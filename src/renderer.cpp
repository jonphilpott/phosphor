// glad.h first — must precede any other GL headers.
#include <glad/glad.h>
#include "renderer.h"
#include "shader_pipeline.h"
#include <cstdio>
#include <cstring>
#include <cassert>
#include <initializer_list>
#include <utility>

// Maximum vertices we'll ever push in a single frame.
// Sized for dense automata grids at Retina resolution:
//   640 cols × 360 rows × 6 verts/rect ≈ 1.38 M verts at 100% density.
// Using 1 M (2^20) as a safe ceiling — 24 MB CPU + 24 MB GPU buffer.
static constexpr int MAX_VERTS = 1048576;

// ── GLSL shaders (inline) ─────────────────────────────────────────────────────
//
// The vertex shader converts from pixel coordinates (top-left origin, +Y down)
// to OpenGL's Normalised Device Coordinates (centre origin, +Y up, -1..1).
//
// The conversion for a screen of width W and height H:
//   NDC_x =  (pixel_x / W) * 2 - 1          (maps 0..W → -1..+1)
//   NDC_y = -(pixel_y / H) * 2 + 1           (maps 0..H → +1..-1, flips Y)

static const char* k_vert = R"glsl(
#version 140

in vec2 a_pos;
in vec4 a_color;

uniform vec2 u_resolution;  // drawable size in pixels

out vec4 v_color;

void main() {
    vec2 ndc = vec2(
        (a_pos.x / u_resolution.x) *  2.0 - 1.0,
        (a_pos.y / u_resolution.y) * -2.0 + 1.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_color = a_color;
}
)glsl";

static const char* k_frag = R"glsl(
#version 140
in  vec4 v_color;
out vec4 frag_color;
void main() {
    frag_color = v_color;
}
)glsl";

// ── Blit shaders ──────────────────────────────────────────────────────────────
//
// Used for:
//   1. draw_feedback()  — blits the persistent feedback texture mid-frame,
//      with an alpha fade and optional scale/rotate transform.
//   2. end_frame() screen blit — copies the post-processed FBO to the screen.
//   3. end_frame() feedback copy — copies the final result into the feedback FBO.
//
// The vertex shader applies a 2D affine transform (mat3) to the quad vertices
// in NDC space.  For a plain passthrough, pass the identity matrix.
// Scale and rotate are applied around the NDC origin (screen centre).

static const char* k_blit_vert = R"glsl(
#version 140

in vec2 a_pos;   // NDC position, -1..1
in vec2 a_uv;    // texture coordinate, 0..1

// Column-major 3×3 matrix for scale + rotate around the screen centre.
// Pass mat3(1.0) (identity) for a plain fullscreen blit.
uniform mat3 u_transform;

out vec2 v_uv;

void main() {
    // Apply the 2D transform in homogeneous NDC space.
    // The w=1 column in mat3 handles the translation row (always 0 here,
    // since we only scale/rotate around the origin).
    vec3 pos = u_transform * vec3(a_pos, 1.0);
    gl_Position = vec4(pos.xy, 0.0, 1.0);
    v_uv = a_uv;
}
)glsl";

static const char* k_blit_frag = R"glsl(
#version 140

uniform sampler2D u_texture;
uniform float     u_alpha;   // overall alpha multiplier (1.0 = fully opaque)

in  vec2 v_uv;
out vec4 frag_color;

void main() {
    vec4 c = texture(u_texture, v_uv);
    frag_color = vec4(c.rgb, c.a * u_alpha);
}
)glsl";

// ── Image shaders ─────────────────────────────────────────────────────────────
//
// Dedicated shaders for drawing textured quads at arbitrary pixel-space rects
// with arbitrary UV sub-ranges.  Used by draw_image() (called from lua_image).
//
// The vertex shader remaps the fullscreen quad (NDC -1..1) to the destination
// pixel rect and interpolates UVs across the specified sub-region.

static const char* k_img_vert = R"glsl(
#version 140

in vec2 a_pos;   // NDC -1..1 (from the shared quad VAO)
in vec2 a_uv;    // not used — we compute UV from uniforms

uniform vec2  u_resolution;   // render-target size in pixels
uniform vec4  u_rect;         // destination rect: x, y, w, h  in pixel space
uniform vec4  u_uv_rect;      // UV sub-range: u0, v0, u1, v1
                               //   (v0 is the BOTTOM UV, v1 is the TOP — OpenGL convention)
uniform float u_angle;        // rotation in radians around the rect's centre (CCW positive)

out vec2 v_uv;

void main() {
    // Step 1: Remap NDC (-1..1) to a normalised [0..1] parameter t.
    // t.x=0 → left edge of quad,  t.x=1 → right edge
    // t.y=0 → NDC bottom,         t.y=1 → NDC top
    vec2 t = a_pos * 0.5 + 0.5;

    // Step 2: Map t to destination pixel position (unrotated).
    // t.y=1 (NDC top) → pixel_y = rect.y      (visual top, since +Y is downward)
    // t.y=0 (NDC bot) → pixel_y = rect.y + rect.w (visual bottom)
    float px = u_rect.x + t.x * u_rect.z;
    float py = u_rect.y + (1.0 - t.y) * u_rect.w;

    // Step 3: Rotate (px, py) around the centre of the destination rect.
    // This is a standard 2-D rotation: the four quad corners orbit the rect
    // centre, so the whole image spins in place.
    // When u_angle == 0: cos=1, sin=0 → px/py unchanged (no branch needed).
    float cx = u_rect.x + u_rect.z * 0.5;
    float cy = u_rect.y + u_rect.w * 0.5;
    float dx = px - cx;
    float dy = py - cy;
    float ca = cos(u_angle);
    float sa = sin(u_angle);
    px = cx + ca * dx - sa * dy;
    py = cy + sa * dx + ca * dy;

    // Step 4: Convert rotated pixel coords to NDC.
    // +Y is UP in NDC, DOWN in pixel space — hence the negation on Y.
    vec2 ndc = vec2(
        px / u_resolution.x * 2.0 - 1.0,
        1.0 - py / u_resolution.y * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);

    // Step 5: UV is based on the un-rotated t (texture coords are local to the
    // quad, so they rotate with it — the texture content appears rotated).
    v_uv = vec2(mix(u_uv_rect.x, u_uv_rect.z, t.x),
                mix(u_uv_rect.y, u_uv_rect.w, t.y));
}
)glsl";

static const char* k_img_frag = R"glsl(
#version 140
uniform sampler2D u_texture;
in  vec2 v_uv;
out vec4 frag_color;
void main() {
    frag_color = texture(u_texture, v_uv);
}
)glsl";

// ── Mat3 implementation ───────────────────────────────────────────────────────

Mat3 Mat3::identity() {
    Mat3 r{};
    // Diagonal of 1s, rest 0.
    r.m[0]=1; r.m[1]=0; r.m[2]=0;
    r.m[3]=0; r.m[4]=1; r.m[5]=0;
    r.m[6]=0; r.m[7]=0; r.m[8]=1;
    return r;
}

Mat3 Mat3::from_translate(float tx, float ty) {
    Mat3 r = identity();
    r.m[2] = tx;
    r.m[5] = ty;
    return r;
}

Mat3 Mat3::from_rotate(float a) {
    // Standard 2D rotation matrix (counter-clockwise by a radians):
    //   [cos(a)  -sin(a)  0]
    //   [sin(a)   cos(a)  0]
    //   [0        0       1]
    float c = cosf(a), s = sinf(a);
    Mat3 r = identity();
    r.m[0]=c;  r.m[1]=-s;
    r.m[3]=s;  r.m[4]= c;
    return r;
}

Mat3 Mat3::from_scale(float sx, float sy) {
    Mat3 r = identity();
    r.m[0] = sx;
    r.m[4] = sy;
    return r;
}

Mat3 Mat3::operator*(const Mat3& o) const {
    // Standard row-major 3×3 multiply: result[i][j] = sum(this[i][k]*o[k][j])
    Mat3 res{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            res.m[row*3+col] =
                m[row*3+0] * o.m[0*3+col] +
                m[row*3+1] * o.m[1*3+col] +
                m[row*3+2] * o.m[2*3+col];
    return res;
}

void Mat3::transform_point(float x, float y, float& ox, float& oy) const {
    // Homogeneous 2D transform: [x' y' 1] = M * [x y 1]
    ox = m[0]*x + m[1]*y + m[2];
    oy = m[3]*x + m[4]*y + m[5];
    // m[6..8] is always [0,0,1] so we skip the divide
}

// ── Shader helpers ────────────────────────────────────────────────────────────

static unsigned int compile(unsigned int type, const char* src) {
    unsigned int id = glCreateShader(type);
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);
    int ok; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(id, 512, nullptr, log);
        fprintf(stderr, "Shader error: %s\n", log);
        glDeleteShader(id); return 0;
    }
    return id;
}

static unsigned int link(unsigned int v, unsigned int f,
    std::initializer_list<std::pair<unsigned int, const char*>> attribs = {})
{
    unsigned int p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    for (const auto& [loc, name] : attribs)
        glBindAttribLocation(p, loc, name);
    glLinkProgram(p);
    int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, 512, nullptr, log);
        fprintf(stderr, "Program link error: %s\n", log);
        glDeleteProgram(p); return 0;
    }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

// ── Renderer::init() ─────────────────────────────────────────────────────────

bool Renderer::init(int w, int h) {
    m_width  = w;
    m_height = h;

    // ── Step 1: Geometry pipeline ─────────────────────────────────────────
    // Reserve CPU buffer upfront to avoid per-frame reallocations.
    m_verts.reserve(MAX_VERTS * 6);

    // Compile the geometry shaders (pixel-coord → NDC, with colour).
    unsigned int vert = compile(GL_VERTEX_SHADER,   k_vert);
    unsigned int frag = compile(GL_FRAGMENT_SHADER, k_frag);
    if (!vert || !frag) return false;
    m_shader_prog = link(vert, frag, {{0, "a_pos"}, {1, "a_color"}});
    if (!m_shader_prog) return false;

    m_u_resolution = glGetUniformLocation(m_shader_prog, "u_resolution");

    // VAO + VBO for the dynamic vertex buffer (geometry drawn by Lua scripts).
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, MAX_VERTS * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    // Attribute 0 — position: 2 floats at offset 0.
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // Attribute 1 — colour: 4 floats at byte offset 8.
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    // ── Step 2: Blit pipeline ─────────────────────────────────────────────
    // Used for draw_feedback(), screen blit, and feedback FBO copy.
    unsigned int bv = compile(GL_VERTEX_SHADER,   k_blit_vert);
    unsigned int bf = compile(GL_FRAGMENT_SHADER, k_blit_frag);
    if (!bv || !bf) return false;
    m_blit_prog = link(bv, bf, {{0, "a_pos"}, {1, "a_uv"}});
    if (!m_blit_prog) return false;

    m_blit_u_texture   = glGetUniformLocation(m_blit_prog, "u_texture");
    m_blit_u_transform = glGetUniformLocation(m_blit_prog, "u_transform");
    m_blit_u_alpha     = glGetUniformLocation(m_blit_prog, "u_alpha");

    // ── Step 2b: Image pipeline ───────────────────────────────────────────
    // Used for draw_image() — draws textured quads at arbitrary pixel rects
    // with arbitrary UV sub-regions (for sprite sheets and image sub-regions).
    unsigned int iv = compile(GL_VERTEX_SHADER,   k_img_vert);
    unsigned int if_ = compile(GL_FRAGMENT_SHADER, k_img_frag);
    if (!iv || !if_) return false;
    m_img_prog = link(iv, if_, {{0, "a_pos"}, {1, "a_uv"}});
    if (!m_img_prog) return false;

    m_img_u_texture    = glGetUniformLocation(m_img_prog, "u_texture");
    m_img_u_resolution = glGetUniformLocation(m_img_prog, "u_resolution");
    m_img_u_rect       = glGetUniformLocation(m_img_prog, "u_rect");
    m_img_u_uv_rect    = glGetUniformLocation(m_img_prog, "u_uv_rect");
    m_img_u_angle      = glGetUniformLocation(m_img_prog, "u_angle");

    // Fullscreen quad geometry: [ndc_x, ndc_y, u, v] — 4 vertices.
    // TRIANGLE_STRIP covers the screen with two triangles.
    static const float k_quad[] = {
        -1.0f, -1.0f,   0.0f, 0.0f,   // bottom-left
         1.0f, -1.0f,   1.0f, 0.0f,   // bottom-right
        -1.0f,  1.0f,   0.0f, 1.0f,   // top-left
         1.0f,  1.0f,   1.0f, 1.0f,   // top-right
    };
    glGenVertexArrays(1, &m_quad_vao);
    glBindVertexArray(m_quad_vao);
    glGenBuffers(1, &m_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_quad), k_quad, GL_STATIC_DRAW);
    // Stride = 4 floats (16 bytes).
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    // ── Step 3: FBO creation ──────────────────────────────────────────────
    if (!create_fbo(w, h, m_fbo[0], m_fbo_tex[0])) return false;
    if (!create_fbo(w, h, m_fbo[1], m_fbo_tex[1])) return false;
    if (!create_fbo(w, h, m_feedback_fbo, m_feedback_tex)) return false;

    // Clear the feedback FBO to opaque black so the first draw_feedback() call
    // blends against black rather than uninitialised GPU memory.
    glBindFramebuffer(GL_FRAMEBUFFER, m_feedback_fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // ── Step 4: Global GL state ───────────────────────────────────────────
    // Alpha blending: SRC_ALPHA / ONE_MINUS_SRC_ALPHA is the standard
    // "painter's algorithm" blend — each new pixel blends over what's there.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Reset transform stack.
    m_stack[0] = Mat3::identity();
    m_stack_top = 0;

    return true;
}

void Renderer::shutdown() {
    // Geometry pipeline
    if (m_vao)         { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_vbo)         { glDeleteBuffers(1, &m_vbo);      m_vbo = 0; }
    if (m_shader_prog) { glDeleteProgram(m_shader_prog);  m_shader_prog = 0; }

    // Blit pipeline
    if (m_quad_vao)  { glDeleteVertexArrays(1, &m_quad_vao); m_quad_vao = 0; }
    if (m_quad_vbo)  { glDeleteBuffers(1, &m_quad_vbo);      m_quad_vbo = 0; }
    if (m_blit_prog) { glDeleteProgram(m_blit_prog);          m_blit_prog = 0; }
    if (m_img_prog)  { glDeleteProgram(m_img_prog);           m_img_prog  = 0; }

    // FBOs
    destroy_fbo(m_fbo[0],      m_fbo_tex[0]);
    destroy_fbo(m_fbo[1],      m_fbo_tex[1]);
    destroy_fbo(m_feedback_fbo, m_feedback_tex);
}

void Renderer::set_size(int w, int h) {
    m_width  = w;
    m_height = h;

    // FBOs are sized to the drawable dimensions.  Recreate them whenever the
    // window is resized so there's no mismatch between FBO and viewport.
    // destroy_fbo() safely handles zeroed handles (no-ops if fbo == 0).
    destroy_fbo(m_fbo[0],      m_fbo_tex[0]);
    destroy_fbo(m_fbo[1],      m_fbo_tex[1]);
    destroy_fbo(m_feedback_fbo, m_feedback_tex);

    create_fbo(w, h, m_fbo[0],      m_fbo_tex[0]);
    create_fbo(w, h, m_fbo[1],      m_fbo_tex[1]);
    create_fbo(w, h, m_feedback_fbo, m_feedback_tex);

    // Re-clear the feedback FBO so there's no garbage from the old texture.
    glBindFramebuffer(GL_FRAMEBUFFER, m_feedback_fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ── Frame lifecycle ───────────────────────────────────────────────────────────

void Renderer::begin_frame() {
    // ── Step 1: Bind the scene FBO as the render target ───────────────────
    // From this point until end_frame(), all GL draws go into FBO[0]'s texture
    // rather than the screen.  This is what makes post-processing possible —
    // we can read the texture back and apply shaders to it before displaying.
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[0]);
    glViewport(0, 0, m_width, m_height);

    // Initialise the render-target stack with the main scene FBO as the base.
    // canvas:begin() will push onto this; canvas:finish() will pop back to it.
    m_target_stack[0] = { m_fbo[0], m_width, m_height };
    m_target_top = 0;

    // Clear to opaque black.  The Lua script's clear() call will override this
    // with the scene's background colour, but we need a defined initial state
    // so that frames without a clear() call don't accumulate garbage.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // ── Step 2: Reset CPU-side state ──────────────────────────────────────
    m_verts.clear();

    m_stack[0]  = Mat3::identity();
    m_stack_top = 0;

    m_fill[0]=m_fill[1]=m_fill[2]=m_fill[3]=1.0f;
    m_stroke[0]=m_stroke[1]=m_stroke[2]=m_stroke[3]=1.0f;
    m_stroke_weight = 1.0f;
    m_circle_segs   = 32;
}

void Renderer::push_target(unsigned int fbo, int w, int h) {
    // Flush any pending geometry into the current target before switching.
    // This preserves draw order — everything submitted before push_target()
    // lands in the old FBO, everything after lands in the new one.
    flush_verts();

    if (m_target_top < MAX_TARGETS - 1) {
        m_target_top++;
        m_target_stack[m_target_top] = { fbo, w, h };
    } else {
        fprintf(stderr, "Renderer::push_target: target stack overflow (max %d)\n",
                MAX_TARGETS);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
}

void Renderer::pop_target() {
    // Flush geometry accumulated in the current (canvas) target first.
    flush_verts();

    if (m_target_top > 0) {
        m_target_top--;
    } else {
        fprintf(stderr, "Renderer::pop_target: stack underflow\n");
        return;
    }

    const RenderTarget& t = m_target_stack[m_target_top];
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, t.w, t.h);
}

void Renderer::end_frame(ShaderPipeline* pipeline, float time, float beat) {
    m_time = time;   // store for canvas shaders to read via get_time()
    // ── Step 1: Flush remaining geometry into FBO[0] ──────────────────────
    // FBO[0] is still bound from begin_frame().  flush_verts() uploads and
    // draws any primitives accumulated since the last flush (or begin_frame).
    flush_verts();

    // ── Step 2: Run the post-process shader pipeline ──────────────────────
    // apply() ping-pongs between FBO[0] and FBO[1] for each shader in the
    // chain, and returns the index (0 or 1) of the FBO holding the result.
    // If the pipeline is empty or null, result_idx stays 0.
    int result_idx = 0;
    if (pipeline && !pipeline->empty()) {
        result_idx = pipeline->apply(m_fbo, m_fbo_tex, m_width, m_height, time, beat);
    }

    // ── Step 3: Blit result to the screen (default framebuffer = 0) ───────
    // Both the screen blit and the feedback copy are pure overwrites — we want
    // the final FBO content to completely replace the destination, not blend
    // over it.  Disable blending here; draw_feedback() re-enables it as needed.
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);

    // Identity transform: the fullscreen quad covers the entire viewport.
    static const float k_identity[9] = {
        1,0,0,
        0,1,0,
        0,0,1
    };
    blit_texture(m_fbo_tex[result_idx], k_identity, 1.0f);

    // ── Step 4: Copy result into the persistent feedback FBO ──────────────
    // The feedback FBO is never auto-cleared — it persists from frame to frame.
    // Copying the final composite here means draw_feedback() in the *next*
    // frame will read a fully post-processed image, including any pipeline effects.
    glBindFramebuffer(GL_FRAMEBUFFER, m_feedback_fbo);
    glViewport(0, 0, m_width, m_height);
    blit_texture(m_fbo_tex[result_idx], k_identity, 1.0f);

    // Restore blending and default framebuffer for the next frame.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_BLEND);
}

// ── Private helpers ───────────────────────────────────────────────────────────

void Renderer::flush() { flush_verts(); }

void Renderer::flush_verts() {
    if (m_verts.empty()) return;

    // Activate the geometry shader and upload the resolution uniform.
    // Use the current render target's dimensions, not the screen dimensions.
    // When drawing into a canvas FBO the target will be smaller than the screen,
    // and pixel-to-NDC conversion must use the canvas size or circles become ellipses.
    const RenderTarget& tgt = m_target_stack[m_target_top];
    glUseProgram(m_shader_prog);
    glUniform2f(m_u_resolution, (float)tgt.w, (float)tgt.h);

    // glBufferSubData reuses the already-allocated VBO rather than requesting
    // new GPU memory — significantly faster than glBufferData each frame.
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    m_verts.size() * sizeof(float),
                    m_verts.data());

    glDrawArrays(GL_TRIANGLES, 0, (int)m_verts.size() / 6);
    glBindVertexArray(0);

    // Clear the buffer — we've uploaded everything.
    m_verts.clear();
    // Reset the overflow flag so the warning fires again if the next frame
    // also exceeds MAX_VERTS — without this it would only ever fire once.
    m_verts_overflow = false;
}

bool Renderer::create_fbo(int w, int h, unsigned int& fbo, unsigned int& tex) {
    // Step 1: Create and configure the colour texture.
    // GL_RGBA8 = 8 bits per channel, 4 channels.  This is the standard choice
    // for a render target — enough precision for 2D visuals.
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // GL_LINEAR gives smooth blending when the blit shader samples between pixels.
    // Without explicitly setting these, the FBO texture is incomplete — the GPU
    // refuses to render into or sample from it.  (This is a common "gotcha".)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Clamp to edge prevents "bleeding" from the opposite edge when sampling
    // near the texture border (relevant for the feedback scale/rotate effects).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Step 2: Create the FBO and attach the texture to its colour slot.
    // An FBO is just a collection of attachments (colour, depth, stencil).
    // Binding the texture to GL_COLOR_ATTACHMENT0 makes draws go into it.
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    // Step 3: Verify the FBO is complete.  "Complete" means all attachments
    // are valid and the combination is supported by the driver.  Missing this
    // check leads to silent corruption that's very hard to debug.
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Renderer: FBO incomplete (status 0x%x)\n", status);
        return false;
    }
    return true;
}

void Renderer::destroy_fbo(unsigned int& fbo, unsigned int& tex) {
    if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
    if (tex) { glDeleteTextures(1, &tex);     tex = 0; }
}

void Renderer::blit_texture(unsigned int tex, const float mat9[9], float alpha) {
    // Assumes the correct destination framebuffer is already bound by the caller.
    glUseProgram(m_blit_prog);

    // mat9 is a column-major 3×3 matrix (9 floats).
    // GL_FALSE = do NOT transpose — our mat9 is already column-major.
    glUniformMatrix3fv(m_blit_u_transform, 1, GL_FALSE, mat9);
    glUniform1f(m_blit_u_alpha,   alpha);
    glUniform1i(m_blit_u_texture, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBindVertexArray(m_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

// ── draw_feedback() ───────────────────────────────────────────────────────────

void Renderer::draw_feedback(float alpha, float scale, float angle) {
    // Step 1: Flush any pending geometry first.
    // Typically draw_feedback() is called before any draw_* calls so this is
    // a no-op, but flushing here is correct even if called mid-frame.
    // This ensures the feedback quad is composited at the right point in the
    // draw order rather than being interleaved with vertex geometry.
    flush_verts();

    // Step 2: Build the 2D transform for scale + rotate around the screen centre.
    // We work in NDC space where the origin is the screen centre.
    //
    // The combined scale-rotate matrix (column-major for OpenGL):
    //   col 0: (c,  s, 0)   where c = scale*cos(angle), s = scale*sin(angle)
    //   col 1: (-s, c, 0)
    //   col 2: (0,  0, 1)
    //
    // This scales the quad by `scale` and rotates it by `angle` radians,
    // both around NDC (0,0) which is the screen centre.
    float c = cosf(angle) * scale;
    float s = sinf(angle) * scale;
    float mat[9] = {
         c,  s, 0,   // column 0
        -s,  c, 0,   // column 1
         0,  0, 1    // column 2
    };

    // Step 3: Blit the feedback texture into the currently-bound FBO (FBO[0]).
    // Alpha blending is already enabled globally, so a u_alpha < 1.0 makes the
    // previous frame's image semi-transparent — each new frame it fades a little.
    blit_texture(m_feedback_tex, mat, alpha);
}

// ── draw_image() ──────────────────────────────────────────────────────────────

void Renderer::draw_image(unsigned int tex,
                          float x, float y, float w, float h,
                          float u0, float v0, float u1, float v1,
                          float angle)
{
    // Step 1: Flush any pending geometry so draw order is preserved.
    // If draw_image() is called between draw_rect() calls, the geometry that
    // came before should appear underneath the image, not after it.
    flush_verts();

    // Step 2: Bind the image shader and set uniforms.
    // u_resolution uses the current render-target dimensions (not the screen)
    // so that canvas-within-canvas draws position correctly.
    const RenderTarget& rt = m_target_stack[m_target_top];
    glUseProgram(m_img_prog);
    glUniform2f(m_img_u_resolution, (float)rt.w, (float)rt.h);
    glUniform4f(m_img_u_rect,    x, y, w, h);
    glUniform4f(m_img_u_uv_rect, u0, v0, u1, v1);
    glUniform1f(m_img_u_angle,   angle);
    glUniform1i(m_img_u_texture, 0);

    // Step 3: Bind the source texture to texture unit 0.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    // Step 4: Draw using the shared fullscreen quad VAO.
    // The vertex shader remaps the quad from NDC to the destination pixel rect.
    glBindVertexArray(m_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
}

// ── Transform stack ───────────────────────────────────────────────────────────

void Renderer::push() {
    if (m_stack_top >= MAX_STACK - 1) {
        fprintf(stderr, "Renderer: matrix stack overflow\n");
        return;
    }
    // Copy current matrix to the next slot, then advance the pointer.
    m_stack[m_stack_top + 1] = m_stack[m_stack_top];
    ++m_stack_top;
}

void Renderer::pop() {
    if (m_stack_top <= 0) {
        fprintf(stderr, "Renderer: matrix stack underflow\n");
        return;
    }
    --m_stack_top;
}

void Renderer::translate(float tx, float ty) {
    // Post-multiply: new = current * T
    // This means translation is applied in the local (already-rotated) space,
    // which matches Processing's push/translate/rotate behaviour.
    current() = current() * Mat3::from_translate(tx, ty);
}

void Renderer::rotate(float radians) {
    current() = current() * Mat3::from_rotate(radians);
}

void Renderer::scale(float sx, float sy) {
    current() = current() * Mat3::from_scale(sx, sy);
}

// ── Colour state ──────────────────────────────────────────────────────────────

void Renderer::set_color(float r, float g, float b, float a) {
    m_fill[0]=r; m_fill[1]=g; m_fill[2]=b; m_fill[3]=a;
}

void Renderer::set_stroke(float r, float g, float b, float a) {
    m_stroke[0]=r; m_stroke[1]=g; m_stroke[2]=b; m_stroke[3]=a;
}

void Renderer::set_stroke_weight(float w) {
    m_stroke_weight = w < 0.5f ? 0.5f : w;
}

void Renderer::set_circle_segments(int n) {
    m_circle_segs = n < 3 ? 3 : n;
}

// ── Vertex helpers ────────────────────────────────────────────────────────────

void Renderer::xform(float x, float y, float& ox, float& oy) const {
    current().transform_point(x, y, ox, oy);
}

void Renderer::push_vert(float x, float y, float r, float g, float b, float a) {
    // Guard: the VBO was allocated for exactly MAX_VERTS vertices.  If we'd
    // exceed that, glBufferSubData would write past the end of the GPU buffer
    // (undefined behaviour).  This can happen with large automata grids at
    // 100% density — drop the extra geometry rather than crashing, but warn
    // once per frame so the developer knows geometry is being lost.
    if ((int)m_verts.size() >= MAX_VERTS * 6) {
        if (!m_verts_overflow) {
            fprintf(stderr,
                "Renderer: vertex buffer full (%d verts) — geometry dropped this frame. "
                "Reduce draw call density or increase MAX_VERTS in renderer.cpp.\n",
                MAX_VERTS);
            m_verts_overflow = true;
        }
        return;
    }

    // Transform from local space to screen space via the current matrix.
    float sx, sy;
    xform(x, y, sx, sy);
    m_verts.push_back(sx);
    m_verts.push_back(sy);
    m_verts.push_back(r);
    m_verts.push_back(g);
    m_verts.push_back(b);
    m_verts.push_back(a);
}

void Renderer::push_vert_fill(float x, float y) {
    push_vert(x, y, m_fill[0], m_fill[1], m_fill[2], m_fill[3]);
}

void Renderer::push_vert_stroke(float x, float y) {
    push_vert(x, y, m_stroke[0], m_stroke[1], m_stroke[2], m_stroke[3]);
}

// ── Drawing primitives ────────────────────────────────────────────────────────

void Renderer::draw_rect(float x, float y, float w, float h) {
    // Decompose the rectangle into 2 triangles (a quad).
    // The 4 corners in local space:
    //   TL(x,y) --- TR(x+w,y)
    //    |               |
    //   BL(x,y+h) -- BR(x+w,y+h)
    //
    // Triangle 1: TL, TR, BR
    push_vert_fill(x,     y);
    push_vert_fill(x + w, y);
    push_vert_fill(x + w, y + h);
    // Triangle 2: TL, BR, BL
    push_vert_fill(x,     y);
    push_vert_fill(x + w, y + h);
    push_vert_fill(x,     y + h);
}

void Renderer::draw_circle(float cx, float cy, float radius) {
    // Build a filled circle as N triangles, each sharing the centre vertex.
    // For each segment i: emit (centre, perimeter[i], perimeter[i+1]).
    float step = (2.0f * 3.14159265f) / (float)m_circle_segs;
    for (int i = 0; i < m_circle_segs; ++i) {
        float a0 = step *  i;
        float a1 = step * (i + 1);
        push_vert_fill(cx, cy);
        push_vert_fill(cx + cosf(a0) * radius, cy + sinf(a0) * radius);
        push_vert_fill(cx + cosf(a1) * radius, cy + sinf(a1) * radius);
    }
}

void Renderer::draw_line(float x1, float y1, float x2, float y2) {
    // Expand the line segment into a quad of width m_stroke_weight.
    // We compute a normal vector perpendicular to the line direction,
    // scale it to half the stroke weight, then offset both endpoints.
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return;  // degenerate line — skip

    // Unit normal perpendicular to the line direction.
    // nx,ny is the perpendicular: rotate (dx,dy) by 90°.
    float nx = (-dy / len) * (m_stroke_weight * 0.5f);
    float ny = ( dx / len) * (m_stroke_weight * 0.5f);

    // 4 corners of the expanded quad:
    //   A (x1+nx, y1+ny)  ----  B (x2+nx, y2+ny)
    //   D (x1-nx, y1-ny)  ----  C (x2-nx, y2-ny)
    push_vert_stroke(x1+nx, y1+ny);
    push_vert_stroke(x2+nx, y2+ny);
    push_vert_stroke(x2-nx, y2-ny);

    push_vert_stroke(x1+nx, y1+ny);
    push_vert_stroke(x2-nx, y2-ny);
    push_vert_stroke(x1-nx, y1-ny);
}

void Renderer::draw_point(float x, float y) {
    // Draw a square of size stroke_weight centred at (x,y).
    float h = m_stroke_weight * 0.5f;
    push_vert_stroke(x-h, y-h);
    push_vert_stroke(x+h, y-h);
    push_vert_stroke(x+h, y+h);

    push_vert_stroke(x-h, y-h);
    push_vert_stroke(x+h, y+h);
    push_vert_stroke(x-h, y+h);
}
