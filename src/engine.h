#pragma once

#include <SDL2/SDL.h>
#include <vector>
#include <string>
#include <ctime>
#include "osc.h"
#include "lua_state.h"
#include "renderer.h"
#include "shader_pipeline.h"

// Engine is the top-level object that owns the window, GL context, and main
// loop.  Subsystems (OSC, Lua, Renderer) are added as members each phase.
//
// Usage:
//   Engine engine(display_index);
//   if (!engine.init()) return 1;
//   engine.run();          // blocks until user quits
class Engine {
public:
    explicit Engine(int display_index);
    ~Engine();

    bool init();
    void run();
    void load_scene(const char* path);
    void toggle_fullscreen();

private:
    void handle_events();

    // Tear down the Lua VM, reinitialise it, and reload the current scene file.
    // Called automatically when the scene file's mtime changes.
    // GPU state (renderer, FBOs, pipeline) is untouched — only Lua is replaced.
    void reload_scene();

    // Fallback — draws a coloured triangle when no scene is loaded.
    void render_fallback();
    void setup_triangle();

    // ── Member data ───────────────────────────────────────────────────────

    int           m_display_index;
    SDL_Window*   m_window     = nullptr;
    SDL_GLContext m_gl_ctx     = nullptr;
    bool          m_running    = false;
    bool          m_fullscreen = false;
    bool          m_has_scene  = false;
    int           m_draw_w     = 0;
    int           m_draw_h     = 0;

    OscServer     m_osc;
    LuaState      m_lua;
    Renderer      m_renderer;
    ShaderPipeline m_pipeline;

    std::vector<OscMessage> m_osc_msgs;

    // Fallback triangle (Phase 1 — removed when scenes are always present)
    unsigned int m_vao         = 0;
    unsigned int m_vbo         = 0;
    unsigned int m_shader_prog = 0;

    Uint64 m_last_ticks  = 0;
    Uint64 m_fps_ticks   = 0;
    int    m_fps_frames  = 0;
    float  m_time        = 0.0f;  // elapsed seconds — forwarded to shaders as u_time
    float  m_beat        = 0.0f;  // beat phase [0..1), set by /beat OSC — forwarded as u_beat

    // ── Hot reload state ──────────────────────────────────────────────────
    std::string m_scene_path;               // path passed to load_scene()
    time_t      m_scene_mtime   = 0;        // mtime at last successful load
    bool        m_reload_pending = false;   // change detected, waiting debounce
    Uint64      m_reload_timer  = 0;        // SDL ticks when change was first detected
};
