#pragma once

#include <vector>
#include <cmath>

// Forward-declare so renderer.h doesn't pull in shader_pipeline.h (and
// transitively glad.h) everywhere.
class ShaderPipeline;

// ── Mat3 ─────────────────────────────────────────────────────────────────────
// A 3×3 row-major matrix for 2D affine transforms.
// We only ever need [a,b,tx / c,d,ty / 0,0,1] so the bottom row is implicit,
// but storing all 9 values keeps the multiply code simple.
//
// Layout:  m[0] m[1] m[2]   (row 0)
//          m[3] m[4] m[5]   (row 1)
//          m[6] m[7] m[8]   (row 2, always 0 0 1)
struct Mat3 {
    float m[9];

    static Mat3 identity();
    static Mat3 from_translate(float tx, float ty);
    static Mat3 from_rotate(float radians);   // counter-clockwise
    static Mat3 from_scale(float sx, float sy);

    // Compose: apply 'other' first, then 'this'  (this * other in column-major)
    Mat3 operator*(const Mat3& o) const;

    // Apply this transform to a 2D point.
    void transform_point(float x, float y, float& ox, float& oy) const;
};

// ── Renderer ─────────────────────────────────────────────────────────────────
// Owns the 2D drawing state: vertex batcher, transform stack, colour state.
//
// Frame lifecycle (called by Engine):
//   renderer.begin_frame()          — reset buffers and state
//   [Lua on_frame runs, calls draw_*]
//   renderer.end_frame()            — upload vertices and draw
//
// All draw_* calls transform vertices through the current matrix and append
// them to the CPU-side vertex buffer.  Nothing is sent to the GPU until
// end_frame().
class Renderer {
public:
    bool init(int w, int h);
    void shutdown();

    // Call once at the start of every frame (before Lua on_frame).
    // Binds the scene FBO and clears it to opaque black.
    void begin_frame();

    // Call once at the end of every frame (after Lua on_frame).
    // Flushes the vertex buffer, runs the post-process pipeline, blits to
    // screen, then copies the result into the persistent feedback FBO.
    //
    // time  — seconds since startup, forwarded to u_time in shaders
    // beat  — beat phase [0..1), forwarded to u_beat in shaders
    void end_frame(ShaderPipeline* pipeline, float time, float beat);

    // Notify the renderer that the drawable size changed (window resize).
    // Recreates all FBOs at the new dimensions.
    void set_size(int w, int h);

    // ── Transform stack ───────────────────────────────────────────────────
    // Mirrors Processing's pushMatrix/popMatrix.  The stack starts with the
    // identity transform.  Transforms compose: if you translate(100,0) then
    // rotate(0.5), points are first rotated then translated.

    void push();                              // copy current matrix, push
    void pop();                               // discard top, restore previous
    void translate(float tx, float ty);
    void rotate(float radians);
    void scale(float sx, float sy);

    // ── Colour state ──────────────────────────────────────────────────────
    // set_color  — fill colour for rects, circles, points
    // set_stroke — line/outline colour for draw_line
    void set_color(float r, float g, float b, float a);
    void set_stroke(float r, float g, float b, float a);

    // Thickness in pixels for draw_line and draw_point.  Default: 1.
    void set_stroke_weight(float w);

    // Number of segments for draw_circle.  Default: 32.
    void set_circle_segments(int n);

    // ── Feedback ──────────────────────────────────────────────────────────
    // Blit the previous frame's final image into the current scene FBO.
    // Call this at the start of on_frame (before any geometry) to create
    // persistent trail effects.
    //
    // alpha — blend weight (0=invisible, 1=fully opaque).  0.85–0.95 gives
    //         a nice fade trail.
    // scale — scale the quad around the screen centre.  >1 zooms in; the
    //         edges of the previous frame extend outward each frame.
    // angle — rotation in radians.  Non-zero creates a spinning spiral trail.
    void draw_feedback(float alpha, float scale, float angle);

    // ── Drawing primitives ────────────────────────────────────────────────
    // All coordinates are in pixel space: (0,0) top-left, +Y downward.
    // All primitives go through the current matrix transform.

    void draw_rect(float x, float y, float w, float h);

    // Draw a textured quad at pixel position (x, y) with size (w, h).
    // u0,v0 = bottom-left UV; u1,v1 = top-right UV (OpenGL convention:
    // v=0 is image bottom, v=1 is image top when loaded with stbi flip).
    // For a full-image draw: u0=0, v0=0, u1=1, v1=1.
    // angle (radians) rotates the quad around its own centre — positive = CCW.
    // Flushes the geometry vertex buffer first to preserve draw order.
    //
    // IMPORTANT: draw_image() does NOT go through the CPU transform matrix.
    // push/translate/rotate/scale have no effect on images or canvases drawn
    // with this call.  Use the angle parameter for rotation instead.
    void draw_image(unsigned int tex,
                    float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1,
                    float angle = 0.0f);

    // Filled circle centred at (cx, cy).
    void draw_circle(float cx, float cy, float radius);

    // Expanded line quad — avoids glLineWidth which is unreliable in core profile.
    void draw_line(float x1, float y1, float x2, float y2);

    // Draws a square of size stroke_weight centred at (x,y).
    void draw_point(float x, float y);

private:
    // Transform (x,y) through the top of the matrix stack.
    void xform(float x, float y, float& ox, float& oy) const;

    // Append one vertex to the CPU buffer (post-transform).
    void push_vert(float x, float y, float r, float g, float b, float a);

    // Append one vertex using the current fill colour.
    void push_vert_fill(float x, float y);

    // Append one vertex using the current stroke colour.
    void push_vert_stroke(float x, float y);

    // Upload and draw the accumulated vertex buffer, then clear it.
    // Called by end_frame() and draw_feedback() (which must flush before blitting).
    void flush_verts();

    // Create one FBO + colour texture at size w×h.
    // The texture uses GL_RGBA8 with GL_LINEAR filtering.
    // Returns false on failure (e.g. incomplete framebuffer).
    bool create_fbo(int w, int h, unsigned int& fbo, unsigned int& tex);

    // Delete and zero an FBO + its colour texture.
    void destroy_fbo(unsigned int& fbo, unsigned int& tex);

    // Blit a texture onto the currently-bound framebuffer using the blit shader.
    // transform_mat9 is a column-major mat3 (9 floats) applied to the quad vertices.
    void blit_texture(unsigned int tex, const float transform_mat9[9], float alpha);

    // ── GPU objects — geometry pipeline ──────────────────────────────────
    unsigned int m_vao          = 0;
    unsigned int m_vbo          = 0;
    unsigned int m_shader_prog  = 0;
    int          m_u_resolution = -1;   // uniform location cache

    // ── GPU objects — FBO infrastructure ─────────────────────────────────
    // Two scene FBOs for ping-pong post-processing:
    //   m_fbo[0]  — receives the Lua scene render each frame
    //   m_fbo[1]  — ping-pong partner for the shader pipeline
    unsigned int m_fbo[2]     = {0, 0};
    unsigned int m_fbo_tex[2] = {0, 0};

    // Persistent feedback FBO — never auto-cleared.
    // Holds the final composited frame from the previous cycle.
    // draw_feedback() reads from m_feedback_tex.
    // end_frame() writes into m_feedback_fbo after all post-processing.
    unsigned int m_feedback_fbo = 0;
    unsigned int m_feedback_tex = 0;

    // Fullscreen quad for FBO blits and draw_feedback.
    // Vertex format: [ndc_x, ndc_y, u, v]
    unsigned int m_quad_vao   = 0;
    unsigned int m_quad_vbo   = 0;

    // Blit shader: samples a texture and outputs it with an optional
    // scale/rotate transform and alpha multiplier.
    unsigned int m_blit_prog        = 0;
    int          m_blit_u_texture   = -1;
    int          m_blit_u_transform = -1;
    int          m_blit_u_alpha     = -1;

    // Image shader: draws a textured quad at a pixel-space rect with a UV sub-range.
    // Used by draw_image() for image.load() and sprite_sheet draws.
    unsigned int m_img_prog         = 0;
    int          m_img_u_texture    = -1;
    int          m_img_u_resolution = -1;
    int          m_img_u_rect       = -1;
    int          m_img_u_uv_rect    = -1;
    int          m_img_u_angle      = -1;

    // ── Render-target stack ───────────────────────────────────────────────
    // push_target() / pop_target() let canvas:begin() / canvas:finish()
    // redirect draw calls to an offscreen FBO and restore the previous target
    // afterwards.  Targets nest: a canvas inside a canvas works correctly.
    //
    // stack[0] is always the main scene FBO (set by begin_frame).
    // push_target flushes pending geometry before switching so draw order
    // is preserved even when straddling an FBO switch.
public:
    void push_target(unsigned int fbo, int w, int h);
    void pop_target();
    float get_time() const { return m_time; }

    // Flush the CPU vertex buffer to the GPU immediately.
    // Normally called automatically by push_target/pop_target/end_frame.
    // Exposed publicly so canvas:finish() can commit geometry before running
    // its local shader pipeline (which rebinds FBOs internally).
    void flush();

private:
    struct RenderTarget { unsigned int fbo; int w, h; };
    static constexpr int MAX_TARGETS = 16;
    RenderTarget m_target_stack[MAX_TARGETS];
    int          m_target_top = 0;

    // ── CPU vertex buffer ─────────────────────────────────────────────────
    // Format per vertex: x, y, r, g, b, a  (6 floats)
    std::vector<float> m_verts;
    // Set to true the first time push_vert() hits MAX_VERTS in a given frame.
    // Cleared by flush_verts() so the warning fires at most once per frame.
    bool m_verts_overflow = false;

    // ── State ─────────────────────────────────────────────────────────────
    int   m_width  = 0;
    int   m_height = 0;
    float m_time   = 0.0f;   // elapsed seconds, set by end_frame(), read by canvas shaders

    float m_fill[4]   = {1,1,1,1};    // current fill colour
    float m_stroke[4] = {1,1,1,1};    // current stroke colour
    float m_stroke_weight = 1.0f;
    int   m_circle_segs   = 32;

    // Transform stack — starts with identity, reset each frame.
    static constexpr int MAX_STACK = 64;
    Mat3 m_stack[MAX_STACK];
    int  m_stack_top = 0;             // index of current matrix

    const Mat3& current() const { return m_stack[m_stack_top]; }
    Mat3&       current()       { return m_stack[m_stack_top]; }
};
