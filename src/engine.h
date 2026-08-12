#pragma once

#include <SDL2/SDL.h>
#include <vector>
#include <string>
#include <ctime>
#include <cstdint>
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
    void request_fullscreen();

    // UDP port to listen for OSC on.  Call before init().
    void set_osc_port(uint16_t port) { m_osc_port = port; }

private:
    void handle_events();

    // Tear down the Lua VM, reinitialise it, and reload the current scene file.
    // Called automatically when the scene file's mtime changes.
    // GPU state (renderer, FBOs, pipeline) is untouched — only Lua is replaced.
    void reload_scene();

    // ── Per-frame steps, in the order run() calls them ────────────────────
    float update_timing();                        // advance clock, fps title
    void  dispatch_osc();                         // drain and route OSC queue
    bool  handle_engine_osc(const OscMessage& m); // true if engine consumed it
    void  poll_hot_reload();                      // reload if the file changed
    void  render_frame(float dt);                 // run on_frame and present

    // Glyph scale for engine-drawn text, proportional to the drawable size.
    float text_scale_for_display() const;

    // Draw the current Lua error across the top of the screen.
    void  draw_error_banner(const std::string& msg);

    // Print a scene error to stderr, throttled to once a second.
    void  log_scene_error(const std::string& msg);

    // ── Member data ───────────────────────────────────────────────────────

    int           m_display_index;
    SDL_Window*   m_window     = nullptr;
    SDL_GLContext m_gl_ctx     = nullptr;
    bool          m_running    = false;
    bool          m_fullscreen         = false;
    bool          m_pending_fullscreen = false;
    bool          m_has_scene  = false;
    int           m_draw_w     = 0;
    int           m_draw_h     = 0;

    OscServer     m_osc;
    uint16_t      m_osc_port = 9000;
    LuaState      m_lua;
    Renderer      m_renderer;
    ShaderPipeline m_pipeline;

    std::vector<OscMessage> m_osc_msgs;

    // ── Transport ─────────────────────────────────────────────────────────
    bool  m_paused     = false;   // space: hold the scene on its current frame
    float m_time_scale = 1.0f;    // [ and ]: how fast time passes

    // ── Scene error state ─────────────────────────────────────────────────
    // Set when on_frame raises; cleared by the first frame that draws cleanly.
    // While set, the engine holds the last good frame and shows the message.
    std::string m_scene_error;
    std::string m_logged_error;      // last message actually printed
    Uint64      m_error_log_ticks = 0;

    // Throttle for shader mtime polling — see poll_hot_reload().
    Uint64      m_shader_poll_ticks = 0;

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
