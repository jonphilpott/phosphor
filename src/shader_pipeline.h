#pragma once

#include <string>
#include <vector>
#include <unordered_map>

// GLuint is a typedef for unsigned int — avoid pulling all of glad.h into
// every translation unit that includes this header.
using GLuint = unsigned int;
using GLint  = int;

// ── ShaderPipeline ────────────────────────────────────────────────────────────
// Manages a chain of fullscreen post-process fragment shaders.
//
// Each shader reads from a source texture and renders into a destination FBO.
// When there are N shaders in the chain, the image ping-pongs between two FBOs
// N times — the output of shader[i] becomes the input of shader[i+1].
//
// The pipeline does NOT own the FBOs — the Renderer creates and resizes them.
// The pipeline owns the GLSL programs and the fullscreen quad geometry.
//
// Frame lifecycle (called from Renderer::end_frame):
//   int result_idx = pipeline.apply(fbos, texs, w, h, time, beat);
//   // result is in fbos[result_idx] / texs[result_idx]
class ShaderPipeline {
public:
    // Initialise GPU objects (quad VAO/VBO).
    // Must be called once after the OpenGL context is created.
    bool init();

    // Delete all GPU objects (programs, VAO, VBO).
    void shutdown();

    // ── Pipeline management ───────────────────────────────────────────────

    // Replace the entire shader chain.  Each name maps to shaders/<name>.frag.
    // Programs are compiled and linked immediately.
    void set(const std::vector<std::string>& names);

    // Append a single shader to the end of the chain.
    void add(const std::string& name);

    // Remove all shaders — pipeline becomes a passthrough (no post-processing).
    void clear();

    // Set a float uniform that will be uploaded to every shader in the chain
    // that exposes a uniform with the given name.
    void set_uniform(const std::string& name, float value);

    // Returns true when the chain is empty (no post-processing will run).
    bool empty() const { return m_shaders.empty(); }

    // ── Rendering ─────────────────────────────────────────────────────────
    // Run the full shader chain using ping-pong FBOs.
    //
    // Input:  fbos[0] and texs[0] must hold the scene render from this frame.
    // Output: returns the index (0 or 1) indicating which fbo/tex has the result.
    //
    // If the pipeline is empty, this returns 0 immediately (no work done).
    int apply(GLuint fbos[2], GLuint texs[2], int w, int h, float time, float beat);

private:
    // Per-entry state: GPU program handle plus cached uniform locations.
    // Caching uniform locations avoids a string lookup every frame.
    struct ShaderEntry {
        std::string name;
        GLuint prog        = 0;
        GLint  u_texture   = -1;
        GLint  u_resolution= -1;
        GLint  u_time      = -1;
        GLint  u_beat      = -1;
    };

    // Compile + link one entry's fragment shader against the shared vertex shader.
    // Reads the .frag source from shaders/<entry.name>.frag.
    // Returns false and prints to stderr on any error.
    bool load_shader(ShaderEntry& entry);

    std::vector<ShaderEntry>               m_shaders;
    std::unordered_map<std::string, float> m_uniforms;

    // Fullscreen quad: 4 vertices, each [ndc_x, ndc_y, u, v].
    // Used for every blit in the post-process chain.
    GLuint m_quad_vao = 0;
    GLuint m_quad_vbo = 0;
};
