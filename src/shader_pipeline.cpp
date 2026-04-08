// glad.h first — must precede any other GL headers.
#include <glad/glad.h>
#include "shader_pipeline.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// ── Shared vertex shader (fullscreen quad) ────────────────────────────────────
//
// Every post-process shader in the pipeline uses this same vertex stage.
// It simply passes NDC positions straight through and forwards the UV
// coordinate to the fragment stage.
//
// The quad covers NDC (-1,-1) → (1,1) which is the entire viewport.
// UV (0,0) = bottom-left of the texture (OpenGL convention).
static const char* k_quad_vert = R"glsl(
#version 330 core

layout(location = 0) in vec2 a_pos;   // NDC position, -1..1
layout(location = 1) in vec2 a_uv;    // Texture coordinate, 0..1

out vec2 v_uv;

void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
)glsl";

// ── File reading helper ───────────────────────────────────────────────────────

// Read a whole text file into a newly malloc'd buffer.
// Returns nullptr on failure.  Caller must free() the result.
static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ShaderPipeline: cannot open '%s'\n", path);
        return nullptr;
    }

    // Seek to end to get file size, then rewind.
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char* buf = (char*)malloc(size + 1);
    if (!buf) { fclose(f); return nullptr; }

    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

// ── GLSL compile + link helpers ───────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);

    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        fprintf(stderr, "ShaderPipeline: shader compile error:\n%s\n", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

static GLuint link_program(GLuint vert, GLuint frag) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "ShaderPipeline: program link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }

    // Individual shader objects are no longer needed once the program is linked.
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

// ── ShaderPipeline::init() ────────────────────────────────────────────────────

bool ShaderPipeline::init() {
    // Build a fullscreen quad covering NDC (-1,-1) to (1,1).
    // Format per vertex: [ndc_x, ndc_y, u, v]  — 4 floats.
    //
    // GL texture convention: (0,0) is bottom-left.
    // NDC (-1,-1) = bottom-left of screen → UV (0,0).  ✓
    //
    // Using TRIANGLE_STRIP: 0→1→2 forms triangle 1, 1→2→3 forms triangle 2.
    //   2 ─── 3
    //   │ \   │
    //   │  \  │
    //   0 ─── 1
    static const float k_quad[] = {
        -1.0f, -1.0f,   0.0f, 0.0f,   // 0  bottom-left
         1.0f, -1.0f,   1.0f, 0.0f,   // 1  bottom-right
        -1.0f,  1.0f,   0.0f, 1.0f,   // 2  top-left
         1.0f,  1.0f,   1.0f, 1.0f,   // 3  top-right
    };

    glGenVertexArrays(1, &m_quad_vao);
    glBindVertexArray(m_quad_vao);

    glGenBuffers(1, &m_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_quad), k_quad, GL_STATIC_DRAW);

    // Stride = 4 floats per vertex (16 bytes).
    // Attribute 0: NDC position — 2 floats at offset 0.
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // Attribute 1: UV — 2 floats at offset 2 floats (8 bytes).
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    return true;
}

// ── ShaderPipeline::shutdown() ────────────────────────────────────────────────

void ShaderPipeline::shutdown() {
    clear();
    if (m_quad_vao) { glDeleteVertexArrays(1, &m_quad_vao); m_quad_vao = 0; }
    if (m_quad_vbo) { glDeleteBuffers(1, &m_quad_vbo);      m_quad_vbo = 0; }
}

// ── ShaderPipeline::load_shader() ────────────────────────────────────────────

bool ShaderPipeline::load_shader(ShaderEntry& entry) {
    // Build path: shaders/<name>.frag  (relative to the working directory,
    // which is the project root when running ./build/phosphor from there).
    char path[256];
    snprintf(path, sizeof(path), "shaders/%s.frag", entry.name.c_str());

    char* frag_src = read_file(path);
    if (!frag_src) return false;

    // Compile the shared quad vertex shader fresh for each program.
    // The driver de-duplicates identical SPIR-V/IR internally so this is cheap.
    GLuint vert = compile_shader(GL_VERTEX_SHADER,   k_quad_vert);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    free(frag_src);

    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return false;
    }

    entry.prog = link_program(vert, frag);  // deletes vert+frag on success
    if (!entry.prog) return false;

    // Cache uniform locations so we avoid string lookups every frame.
    // glGetUniformLocation returns -1 if the name doesn't exist — that's fine,
    // we check for -1 before uploading.
    entry.u_texture    = glGetUniformLocation(entry.prog, "u_texture");
    entry.u_resolution = glGetUniformLocation(entry.prog, "u_resolution");
    entry.u_time       = glGetUniformLocation(entry.prog, "u_time");
    entry.u_beat       = glGetUniformLocation(entry.prog, "u_beat");

    return true;
}

// ── ShaderPipeline::set / add / clear ─────────────────────────────────────────

void ShaderPipeline::set(const std::vector<std::string>& names) {
    clear();
    for (const auto& name : names) add(name);
}

void ShaderPipeline::add(const std::string& name) {
    ShaderEntry entry;
    entry.name = name;
    if (load_shader(entry)) {
        m_shaders.push_back(entry);
    }
    // If load fails, the error was already printed — silently skip the shader.
}

void ShaderPipeline::clear() {
    for (auto& s : m_shaders) {
        if (s.prog) glDeleteProgram(s.prog);
    }
    m_shaders.clear();
}

void ShaderPipeline::set_uniform(const std::string& name, float value) {
    m_uniforms[name] = value;
}

// ── ShaderPipeline::apply() ───────────────────────────────────────────────────

int ShaderPipeline::apply(GLuint fbos[2], GLuint texs[2], int w, int h,
                           float time, float beat)
{
    if (m_shaders.empty()) return 0;

    // Post-process passes are pure texture-to-texture overwrites — alpha blending
    // must be OFF.  With blending on, the destination FBO's own cleared colour
    // (and its alpha channel) bleeds through whenever the source has alpha < 1,
    // which corrupts every pass in the chain.  We restore blending afterwards so
    // the rest of the frame (geometry, draw_feedback) is unaffected.
    glDisable(GL_BLEND);

    int src = 0;

    glBindVertexArray(m_quad_vao);

    for (auto& shader : m_shaders) {
        // dst is the other FBO — we ping-pong: 0→1→0→1…
        int dst = 1 - src;

        // ── Critical ordering: bind source texture BEFORE destination FBO ──
        //
        // OpenGL forbids a "feedback loop": a texture may not be simultaneously
        // attached to the bound draw-FBO AND bound to a texture unit.  If the
        // loop is open when the FBO is bound, the framebuffer becomes incomplete
        // and the subsequent glClear / glDrawArrays silently do nothing.
        //
        // The hazard arises in pass 2 (src=1, dst=0): after pass 1, texture
        // unit 0 still holds m_fbo_tex[0].  If we bound FBO[0] (whose colour
        // attachment IS m_fbo_tex[0]) first, the loop would open for the
        // window between glBindFramebuffer and glBindTexture.
        //
        // By binding the source texture first, unit 0 always holds texs[src]
        // (which is NEVER the attachment of fbos[dst] in a ping-pong pair)
        // before the destination FBO is bound.  No feedback loop, ever.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texs[src]);   // ← must come first

        glBindFramebuffer(GL_FRAMEBUFFER, fbos[dst]);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(shader.prog);

        // Upload standard uniforms (check for -1 to skip missing ones).
        if (shader.u_texture   >= 0) glUniform1i(shader.u_texture, 0);
        if (shader.u_resolution>= 0) glUniform2f(shader.u_resolution, (float)w, (float)h);
        if (shader.u_time      >= 0) glUniform1f(shader.u_time, time);
        if (shader.u_beat      >= 0) glUniform1f(shader.u_beat, beat);

        // Upload any custom float uniforms the Lua scene has set.
        for (const auto& kv : m_uniforms) {
            GLint loc = glGetUniformLocation(shader.prog, kv.first.c_str());
            if (loc >= 0) glUniform1f(loc, kv.second);
        }

        // Draw the fullscreen quad — this runs the fragment shader over every pixel.
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        src = dst;  // the result is now in dst; swap for next iteration
    }

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Restore blending for subsequent geometry / draw_feedback draws.
    glEnable(GL_BLEND);

    return src;  // caller knows which FBO/texture has the final image
}
