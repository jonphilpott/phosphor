#include "engine.h"
#include "lua_bindings.h"
#include "lua_text.h"   // text_render::draw — for the "no scene loaded" message
#include "gl_utils.h"

// glad.h MUST be included before any SDL or system OpenGL headers.
// GLAD defines the actual GL function pointer stubs — including it after
// another header that pulls in gl.h will cause "already defined" errors.
#include <glad/glad.h>

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>      // realpath()
#include <climits>      // PATH_MAX
#include <sys/stat.h>   // stat() for file mtime polling

// Return the modification time of a file, or 0 if it can't be stat'd.
// stat() is POSIX — works on macOS and Linux with no extra dependencies.
static time_t file_mtime(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0) ? st.st_mtime : 0;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

Engine::Engine(int display_index)
    : m_display_index(display_index)
{}

Engine::~Engine() {
    // Clean up in reverse order of creation.
    m_pipeline.shutdown();
    m_renderer.shutdown();
    if (m_gl_ctx)       SDL_GL_DeleteContext(m_gl_ctx);
    if (m_window)       SDL_DestroyWindow(m_window);
    SDL_Quit();
}

// ── init() ────────────────────────────────────────────────────────────────────

bool Engine::init() {
    // Step 1: Initialise SDL's video subsystem.
    // SDL_INIT_VIDEO also initialises the events subsystem automatically.
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // Step 2: Request an OpenGL 3.1 Core Profile context.
    //
    // These attributes MUST be set before SDL_CreateWindow — SDL reads them
    // when it creates the window's pixel format.
    //
    // Core Profile means we get only the modern API; legacy functions like
    // glBegin/glEnd are removed.  This is what we want — it forces good
    // habits and is the only profile guaranteed on macOS.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,  SDL_GL_CONTEXT_PROFILE_CORE);

    // macOS REQUIRES the forward-compatible flag to get a Core Profile context.
    // Without it, macOS silently falls back to a legacy 2.1 context and you get
    // cryptic "invalid operation" errors when using VAOs or modern shaders.
    // On Linux this flag is harmless — it just promises we won't use deprecated
    // features, which we don't anyway.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);

    // Double buffering: one buffer is displayed while we draw into the other,
    // then we swap them — prevents tearing.
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // Step 3: Validate the requested display index.
    int num_displays = SDL_GetNumVideoDisplays();
    if (m_display_index >= num_displays) {
        fprintf(stderr, "Display %d not found (%d display(s) available). Using 0.\n",
                m_display_index, num_displays);
        m_display_index = 0;
    }

    // Step 4: Create the window.
    //
    // SDL_WINDOWPOS_CENTERED_DISPLAY(n) centres the window on display n.
    // SDL_WINDOW_OPENGL tells SDL this window will have a GL context.
    // SDL_WINDOW_ALLOW_HIGHDPI opts into Retina/HiDPI rendering on macOS —
    // the drawable size in pixels will be 2x the logical window size.
    // SDL_WINDOW_RESIZABLE lets us resize during prototyping.
    const int WIN_W = 1280;
    const int WIN_H = 720;

    m_window = SDL_CreateWindow(
        "phosphor",
        SDL_WINDOWPOS_CENTERED_DISPLAY(m_display_index),
        SDL_WINDOWPOS_CENTERED_DISPLAY(m_display_index),
        WIN_W, WIN_H,
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
    );
    if (!m_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    // Step 5: Create the OpenGL context and make it current on our window.
    m_gl_ctx = SDL_GL_CreateContext(m_window);
    if (!m_gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(m_window, m_gl_ctx);

    // Step 6: Load all OpenGL function pointers via GLAD.
    //
    // OpenGL functions are not linked statically — the driver provides them
    // at runtime.  GLAD uses SDL_GL_GetProcAddress to find each function by
    // name and stores the pointer.  After this call, every gl* function works.
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "gladLoadGLLoader failed — could not load GL function pointers\n");
        return false;
    }

    // Verify we actually got the version we asked for.
    if (!GLAD_GL_VERSION_3_1) {
        fprintf(stderr, "OpenGL 3.1 not supported on this system\n");
        return false;
    }
    printf("OpenGL %s — %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    // Step 7: Enable vsync (swap interval = 1 means wait for one vertical
    // blank before swapping).  This caps us at the display refresh rate and
    // prevents screen tearing.  Returns -1 if adaptive vsync is unsupported,
    // but interval 1 is universally available.
    SDL_GL_SetSwapInterval(1);

    // Step 8: Get the actual drawable pixel size.
    // On a Retina display the logical window is 1280x720 but the drawable
    // framebuffer is 2560x1440 — we must use the drawable size for glViewport.
    SDL_GL_GetDrawableSize(m_window, &m_draw_w, &m_draw_h);
    glViewport(0, 0, m_draw_w, m_draw_h);

    // Step 10: Initialise the 2D renderer.
    if (!m_renderer.init(m_draw_w, m_draw_h)) {
        fprintf(stderr, "Renderer init failed\n");
        return false;
    }

    // Step 10b: Initialise the shader pipeline (builds the fullscreen quad VAO).
    // Must happen after gladLoadGLLoader so GL functions are available.
    if (!m_pipeline.init()) {
        fprintf(stderr, "ShaderPipeline init failed\n");
        return false;
    }

    // Step 11: Initialise the Lua VM and register engine bindings.
    if (!m_lua.init()) {
        fprintf(stderr, "Lua init failed\n");
        return false;
    }
    // Give bindings access to the renderer and pipeline via the Lua registry.
    lua_bindings::set_renderer(m_lua.L, &m_renderer);
    lua_bindings::set_pipeline(m_lua.L, &m_pipeline);
    // Push drawable dimensions as Lua globals (screen_width, screen_height).
    m_lua.set_screen_size(m_draw_w, m_draw_h);

    // Step 11: Start the OSC server on port 9000.
    // The server binds a UDP socket and spawns a recv thread.
    // All clients (SC, PD, TouchOSC) send to this same port simultaneously —
    // UDP is connectionless so there's no concept of "one connection per client".
    if (!m_osc.start(9000)) {
        fprintf(stderr, "Warning: OSC server failed to start — continuing without OSC\n");
    }

    // Seed the C random number generator.  conway:randomize() and
    // wolfram:randomize() call rand(); without a seed the C library starts from
    // the same state on every launch, so those scenes came up with the byte-for
    // -byte identical "random" pattern every single time they were run.
    // (Lua's math.random has its own generator and its own seeding — this is
    // only about the C-side automata.)
    srand((unsigned)time(nullptr));

    // Initialise the frame timer.
    m_last_ticks = m_fps_ticks = SDL_GetTicks64();

    printf("Display %d — drawable %dx%d px\n", m_display_index, m_draw_w, m_draw_h);
    printf("F = toggle fullscreen   Esc = quit\n");

    m_running = true;
    return true;
}

// ── load_scene() ─────────────────────────────────────────────────────────────

void Engine::load_scene(const char* path) {
    m_scene_path    = path;
    m_scene_mtime   = file_mtime(path);
    m_reload_pending = false;

    if (m_lua.load_file(path)) {
        m_has_scene = true;
        m_lua.call_hook("on_load");
    }
}

// ── set_scene_root() ─────────────────────────────────────────────────────────
//
// The scene root is the one directory that OSC-loaded scenes are allowed to
// come from.  We resolve it once, at startup, with realpath() so that all later
// comparisons are between two fully-resolved absolute paths — no "../" left to
// interpret, no symlinks left to follow.

void Engine::set_scene_root(const char* scene_path) {
    // Derive the directory from the scene path: everything before the last '/'.
    std::string dir = ".";
    if (scene_path) {
        const char* slash = strrchr(scene_path, '/');
        if (slash) {
            dir.assign(scene_path, (size_t)(slash - scene_path));
            if (dir.empty()) dir = "/";   // scene was at the filesystem root
        }
        // No slash at all means the scene sits in the working directory,
        // so "." is already correct.
    }

    char resolved[PATH_MAX];
    if (realpath(dir.c_str(), resolved)) {
        m_scene_root = resolved;
    } else {
        // Leaving the root empty makes handle_scene_request reject everything,
        // which is the safe direction to fail in.
        m_scene_root.clear();
        fprintf(stderr, "Warning: could not resolve scene directory '%s' — "
                        "OSC /scene will be refused\n", dir.c_str());
    }
}

// ── handle_scene_request() ───────────────────────────────────────────────────
//
// /scene is the one OSC address that causes code to be executed, so it gets
// three independent checks rather than one.  Any of them failing is logged and
// ignored — we never want a bad message to interrupt a running scene.

void Engine::handle_scene_request(const std::string& path, bool from_loopback) {
    // Check 1: who sent it.  A scene file is arbitrary Lua with the full
    // standard library behind it, so by default only programs on this machine
    // may swap it.  /beat and ordinary scene messages are unaffected by this.
    if (!from_loopback && !m_allow_remote_scene) {
        fprintf(stderr, "OSC /scene: refused '%s' — remote scene loading is "
                        "disabled (pass --allow-remote-scene to permit it)\n",
                path.c_str());
        return;
    }

    // Check 2: it must actually be a scene file.
    if (path.size() < 5 || path.compare(path.size() - 4, 4, ".lua") != 0) {
        fprintf(stderr, "OSC /scene: refused '%s' — not a .lua file\n", path.c_str());
        return;
    }

    if (m_scene_root.empty()) {
        fprintf(stderr, "OSC /scene: refused '%s' — no scene root established\n",
                path.c_str());
        return;
    }

    // Check 3: where it lands.  realpath() collapses "..", resolves symlinks
    // and fails outright if the file doesn't exist — so what comes back is the
    // real file that would be opened, not the string somebody sent us.
    char resolved[PATH_MAX];
    if (!realpath(path.c_str(), resolved)) {
        fprintf(stderr, "OSC /scene: refused '%s' — no such file\n", path.c_str());
        return;
    }

    // Containment test.  The trailing '/' on the prefix matters: without it,
    // a sibling directory whose name merely starts with the root's name (say
    // /gigs/scenes-backup next to /gigs/scenes) would pass the check.
    const std::string prefix = m_scene_root + "/";
    if (strncmp(resolved, prefix.c_str(), prefix.size()) != 0) {
        fprintf(stderr, "OSC /scene: refused '%s' — resolves to '%s', outside "
                        "the scene directory '%s'\n",
                path.c_str(), resolved, m_scene_root.c_str());
        return;
    }

    load_scene(resolved);
}

void Engine::reload_scene() {
    printf("[hot reload] %s\n", m_scene_path.c_str());

    // Step 1: Tear down the Lua VM and create a fresh one.
    // reset() calls lua_close (running __gc on all live userdata — freeing any
    // canvas FBOs or image textures the scene allocated) then calls init().
    m_lua.reset();

    // Step 1b: Reset the post-process pipeline.
    // The pipeline is GPU-side state and so survives the Lua reset, but it was
    // configured by the *old* scene: its shader chain and its accumulated
    // uniform values both belong to code that no longer exists.  Without this,
    // a reloaded scene that doesn't call shader_set() inherits the previous
    // scene's shaders, and the uniform map grows with every reload of the day.
    // The new scene's on_load() sets up whatever it actually wants.
    m_pipeline.clear();

    // Step 2: Re-wire the engine bindings into the new VM.
    // The renderer and pipeline live on the Engine and are completely untouched
    // by the Lua reset — we just re-register their pointers.
    lua_bindings::set_renderer(m_lua.L, &m_renderer);
    lua_bindings::set_pipeline(m_lua.L, &m_pipeline);
    m_lua.set_screen_size(m_draw_w, m_draw_h);

    // Step 3: Reload the scene file.
    m_has_scene = false;
    if (m_lua.load_file(m_scene_path.c_str())) {
        m_has_scene = true;
        m_lua.call_hook("on_load");
    }

    // Step 4: Update the stored mtime so we don't immediately re-trigger.
    m_scene_mtime    = file_mtime(m_scene_path.c_str());
    m_reload_pending = false;
}

// ── run() — main loop ─────────────────────────────────────────────────────────
//
// Each frame is the same five steps, in this order:
//   1. work out how much time passed        — update_timing()
//   2. act on anything that arrived by OSC  — dispatch_osc()
//   3. reload the scene if its file changed — poll_hot_reload()
//   4. handle window and keyboard events    — handle_events()
//   5. draw                                 — render_frame()

void Engine::run() {
    while (m_running) {
        const float dt = update_timing();

        dispatch_osc();
        poll_hot_reload();
        handle_events();
        render_frame(dt);

        // Swap the back buffer to the screen (respects the vsync interval).
        SDL_GL_SwapWindow(m_window);

        // Deferred startup fullscreen — only fires once, on the first frame.
        //
        // Under vsync, SDL_GL_SwapWindow blocks until the compositor's frame
        // callback fires, which means by the time we reach here the compositor
        // has already composited our frame and sent wl_surface.enter.  SDL now
        // holds the correct wl_output reference and will pass it to
        // xdg_toplevel_set_fullscreen instead of NULL, so the compositor puts
        // us on the right display rather than defaulting to display 0.
        if (m_pending_fullscreen) {
            m_pending_fullscreen = false;
            SDL_PumpEvents();   // flush wl_surface.enter into SDL's output list
            toggle_fullscreen();
        }
    }
}

// ── update_timing() ───────────────────────────────────────────────────────────
// Advances the frame clock and refreshes the fps readout in the title bar.
// Returns the delta-time in seconds to hand to the scene.

float Engine::update_timing() {
    // SDL_GetTicks64() returns milliseconds — divide by 1000 for seconds.
    const Uint64 now = SDL_GetTicks64();
    float dt = (now - m_last_ticks) / 1000.0f;
    m_last_ticks = now;

    // Clamp dt to a sane frame.  Anything that stalls the loop — a hot reload,
    // dragging the window, the compositor suspending us — produces one enormous
    // dt on the frame afterwards.  Scenes integrate dt, so an unclamped spike
    // makes everything leap forward: rotations jump, particles teleport off
    // screen, and physics-ish scenes never recover.  Better to lose a little
    // wall-clock accuracy than to lurch on stage.
    const float MAX_DT = 0.1f;   // 10 fps worth — above this we run normally
    if (dt > MAX_DT) dt = MAX_DT;
    if (dt < 0.0f)   dt = 0.0f;  // guard against the clock going backwards

    m_time += dt;

    // FPS counter — update the window title once per second.
    m_fps_frames++;
    if (now - m_fps_ticks >= 1000) {
        char title[64];
        snprintf(title, sizeof(title), "phosphor — %d fps", m_fps_frames);
        SDL_SetWindowTitle(m_window, title);
        m_fps_frames = 0;
        m_fps_ticks  = now;
    }

    return dt;
}

// ── dispatch_osc() ────────────────────────────────────────────────────────────
// Drains the OSC queue and routes each message.  All dispatching happens here on
// the main thread, so Lua callbacks never race with the recv thread filling the
// queue.

void Engine::dispatch_osc() {
    m_osc.poll(m_osc_msgs);

    for (const auto& msg : m_osc_msgs) {
        // Engine-level addresses are intercepted before Lua dispatch, so they
        // work regardless of whether the current scene defines on_osc — or even
        // if that scene has crashed.
        if (handle_engine_osc(msg)) continue;

        // Look up the global function on_osc.  If it isn't defined in the
        // current scene, silently skip — not every scene needs OSC input.
        if (lua_getglobal(m_lua.L, "on_osc") != LUA_TFUNCTION) {
            lua_pop(m_lua.L, 1);
            continue;
        }

        // Push address then each argument in order.
        // OSC int/float → Lua number, OSC string → Lua string.
        // Other types (blob, timetag, MIDI) were already filtered out by the
        // parser in osc_parse.cpp, so we only see 'i', 'f', 's' here.
        lua_pushstring(m_lua.L, msg.address.c_str());
        for (const auto& arg : msg.args) {
            if      (arg.type == 'i') lua_pushinteger(m_lua.L, arg.i);
            else if (arg.type == 'f') lua_pushnumber(m_lua.L,  arg.f);
            else if (arg.type == 's') lua_pushstring(m_lua.L,  arg.s.c_str());
        }

        const int nargs = 1 + (int)msg.args.size();
        if (lua_pcall(m_lua.L, nargs, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(m_lua.L, -1);
            fprintf(stderr, "Lua error [on_osc %s]: %s\n",
                    msg.address.c_str(), err ? err : "(no message)");
            lua_pop(m_lua.L, 1);
        }
    }
}

// ── handle_engine_osc() ───────────────────────────────────────────────────────
// Handles the OSC addresses the engine reserves for itself.
// Returns true if the message was consumed and must not reach on_osc.

bool Engine::handle_engine_osc(const OscMessage& msg) {
    // ── /scene <path> ────────────────────────────────────────────────────────
    // Swap the running scene, e.g. /scene "scenes/matrix.lua".
    // handle_scene_request does the security checks — see its definition.
    if (msg.address == "/scene") {
        if (!msg.args.empty() && msg.args[0].type == 's') {
            handle_scene_request(msg.args[0].s, msg.from_loopback);
        }
        return true;
    }

    // ── /beat <phase> ────────────────────────────────────────────────────────
    // Updates m_beat (→ u_beat in all shaders) and fires on_beat(phase).
    // phase is a float the caller defines — common convention is [0..1) where
    // 0 = downbeat, or a raw beat counter from the sequencer clock.
    // SuperCollider example:
    //   TempoClock.default.schedAbs(TempoClock.default.nextBar, {
    //       ~p.sendMsg("/beat", 0.0); nil });
    if (msg.address == "/beat") {
        if (!msg.args.empty()) {
            if      (msg.args[0].type == 'f') m_beat = msg.args[0].f;
            else if (msg.args[0].type == 'i') m_beat = (float)msg.args[0].i;
        }
        // Fire the optional on_beat(phase) Lua hook.
        if (lua_getglobal(m_lua.L, "on_beat") == LUA_TFUNCTION) {
            lua_pushnumber(m_lua.L, (double)m_beat);
            if (lua_pcall(m_lua.L, 1, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(m_lua.L, -1);
                fprintf(stderr, "Lua error [on_beat]: %s\n",
                        err ? err : "(no message)");
                lua_pop(m_lua.L, 1);
            }
        } else {
            lua_pop(m_lua.L, 1);
        }
        return true;
    }

    return false;   // not ours — pass it to the scene
}

// ── poll_hot_reload() ─────────────────────────────────────────────────────────
// Watches the scene file's modification time and reloads when it changes.
// stat() is a single cheap syscall, so checking every frame is fine.  The 150 ms
// debounce exists because editors write files in several steps (truncate, then
// write) and we must not reload a half-written file.

void Engine::poll_hot_reload() {
    if (m_scene_path.empty()) return;

    const time_t new_mtime = file_mtime(m_scene_path.c_str());
    if (new_mtime == 0 || new_mtime == m_scene_mtime) return;

    if (!m_reload_pending) {
        // First frame where we noticed a change — start the debounce timer.
        m_reload_pending = true;
        m_reload_timer   = SDL_GetTicks64();
    } else if (SDL_GetTicks64() - m_reload_timer >= 150) {
        reload_scene();
    }
}

// ── render_frame() ────────────────────────────────────────────────────────────
// Runs the scene's on_frame hook between the renderer's begin/end, so all its
// draw calls land in the scene FBO and go through the post-process pipeline.

void Engine::render_frame(float dt) {
    if (!m_has_scene) {
        // No scene loaded — say so on screen rather than leaving the window
        // black, which is indistinguishable from a scene that draws nothing.
        m_renderer.begin_frame();
        m_renderer.set_color(0.0f, 0.9f, 0.4f, 1.0f);
        text_render::draw(m_renderer, 24.0f, 24.0f,
                          "phosphor\n\nno scene loaded\n"
                          "run with:  phosphor -s scenes/test.lua", 2.0f);
        m_renderer.end_frame(&m_pipeline, m_time, m_beat);
        return;
    }

    // Reset the renderer's CPU vertex buffer and transform stack for this frame.
    m_renderer.begin_frame();

    // Call the Lua on_frame(dt) hook.  Scripts call clear(), draw_rect(), etc.
    // — these accumulate into the renderer's vertex buffer.
    m_lua.call_hook("on_frame", (double)dt);

    // Flush vertices, run the post-process pipeline, blit to the screen, then
    // copy the result into the feedback FBO for the next frame.
    m_renderer.end_frame(&m_pipeline, m_time, m_beat);
}

// ── handle_events() ───────────────────────────────────────────────────────────

void Engine::handle_events() {
    SDL_Event ev;
    // SDL_PollEvent is non-blocking — drains all pending events each frame.
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                m_running = false;
                break;

            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: m_running = false;     break;
                    case SDLK_f:      toggle_fullscreen();   break;
                }
                break;

            case SDL_WINDOWEVENT:
                // The drawable size can change when the window is resized or
                // moved between displays with different DPI scales.
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GL_GetDrawableSize(m_window, &m_draw_w, &m_draw_h);
                    glViewport(0, 0, m_draw_w, m_draw_h);
                    m_renderer.set_size(m_draw_w, m_draw_h);
                    m_lua.set_screen_size(m_draw_w, m_draw_h);
                }
                break;
        }
    }
}

// ── toggle_fullscreen() ───────────────────────────────────────────────────────

void Engine::toggle_fullscreen() {
    m_fullscreen = !m_fullscreen;

    // SDL_WINDOW_FULLSCREEN_DESKTOP takes over the display without changing
    // the video mode — the compositor simply hides the desktop behind us.
    // This is preferable to SDL_WINDOW_FULLSCREEN which would actually switch
    // resolution and could disrupt the external monitor arrangement.
    Uint32 flags = m_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    SDL_SetWindowFullscreen(m_window, flags);

    // Pump events so SDL collects the window resize notification that the
    // fullscreen transition triggers before we query the new drawable size.
    SDL_PumpEvents();

    // Update the viewport, renderer FBOs, and Lua screen globals together.
    // SDL_WINDOWEVENT_SIZE_CHANGED may also fire and call these again — that's
    // harmless since set_size / set_screen_size are idempotent with equal values.
    SDL_GL_GetDrawableSize(m_window, &m_draw_w, &m_draw_h);
    glViewport(0, 0, m_draw_w, m_draw_h);
    m_renderer.set_size(m_draw_w, m_draw_h);
    m_lua.set_screen_size(m_draw_w, m_draw_h);
}

// ── request_fullscreen() ─────────────────────────────────────────────────────
//
// Called from main() when -f is passed.  We do NOT call toggle_fullscreen()
// directly here because on Wayland the compositor sends wl_surface.enter
// (telling SDL which output the window is on) only AFTER the window's first
// frame has been composited.  Calling fullscreen before that event arrives
// means SDL passes NULL for the wl_output in xdg_toplevel_set_fullscreen,
// and the compositor picks display 0 regardless of where the window was placed.
//
// Setting this flag instead defers the call to run(), where it fires
// after the first SDL_GL_SwapWindow — at which point vsync guarantees the
// compositor has already processed our frame and sent wl_surface.enter.

void Engine::request_fullscreen() {
    m_pending_fullscreen = true;
}
