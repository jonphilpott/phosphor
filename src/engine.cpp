#include "engine.h"
#include "lua_bindings.h"

// glad.h MUST be included before any SDL or system OpenGL headers.
// GLAD defines the actual GL function pointer stubs — including it after
// another header that pulls in gl.h will cause "already defined" errors.
#include <glad/glad.h>

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>   // stat() for file mtime polling

// Return the modification time of a file, or 0 if it can't be stat'd.
// stat() is POSIX — works on macOS and Linux with no extra dependencies.
static time_t file_mtime(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0) ? st.st_mtime : 0;
}

// ── GLSL source for the Phase 1 demo triangle ────────────────────────────────
// These are plain C strings embedded in the binary.  In Phase 4 we will load
// shaders from .glsl files instead.
//
// "layout(location = 0)" binds the attribute to slot 0 — this must match the
// glVertexAttribPointer calls in setup_triangle() below.

static const char* k_vert_src = R"glsl(
#version 330 core

// Slot 0: 2D position in Normalised Device Coordinates (-1..1 on each axis).
layout(location = 0) in vec2 a_pos;

// Slot 1: RGB colour per vertex — we interpolate this across the triangle.
layout(location = 1) in vec3 a_color;

out vec3 v_color;   // passed through to the fragment shader

void main() {
    // In NDC, (0,0) is the centre of the screen, (1,1) is top-right.
    // No projection matrix needed for this 2D demo.
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_color = a_color;
}
)glsl";

static const char* k_frag_src = R"glsl(
#version 330 core

in  vec3 v_color;
out vec4 frag_color;

void main() {
    frag_color = vec4(v_color, 1.0);
}
)glsl";

// ── Triangle vertex data ──────────────────────────────────────────────────────
// Each row: x, y (NDC), r, g, b
// The three vertices form a classic coloured triangle.
static const float k_triangle[] = {
     0.0f,  0.5f,   1.0f, 0.0f, 0.0f,   // top    — red
    -0.5f, -0.5f,   0.0f, 1.0f, 0.0f,   // left   — green
     0.5f, -0.5f,   0.0f, 0.0f, 1.0f,   // right  — blue
};

// ── Constructor / Destructor ──────────────────────────────────────────────────

Engine::Engine(int display_index)
    : m_display_index(display_index)
{}

Engine::~Engine() {
    // Clean up in reverse order of creation.
    m_pipeline.shutdown();
    m_renderer.shutdown();
    if (m_vao)          glDeleteVertexArrays(1, &m_vao);
    if (m_vbo)          glDeleteBuffers(1, &m_vbo);
    if (m_shader_prog)  glDeleteProgram(m_shader_prog);
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

    // Step 2: Request an OpenGL 3.3 Core Profile context.
    //
    // These attributes MUST be set before SDL_CreateWindow — SDL reads them
    // when it creates the window's pixel format.
    //
    // Core Profile means we get only the modern API; legacy functions like
    // glBegin/glEnd are removed.  This is what we want — it forces good
    // habits and is the only profile guaranteed on macOS.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
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
    if (!GLAD_GL_VERSION_3_3) {
        fprintf(stderr, "OpenGL 3.3 not supported on this system\n");
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

    // Step 9: Upload the demo triangle to the GPU.
    setup_triangle();

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

void Engine::reload_scene() {
    printf("[hot reload] %s\n", m_scene_path.c_str());

    // Step 1: Tear down the Lua VM and create a fresh one.
    // reset() calls lua_close (running __gc on all live userdata — freeing any
    // canvas FBOs or image textures the scene allocated) then calls init().
    m_lua.reset();

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

void Engine::run() {
    while (m_running) {
        // Compute delta-time in seconds since the last frame.
        // SDL_GetTicks64() returns milliseconds — divide by 1000 for seconds.
        Uint64 now = SDL_GetTicks64();
        float dt   = (now - m_last_ticks) / 1000.0f;
        m_last_ticks = now;
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

        // Drain the OSC queue — all messages received since the last frame.
        // All dispatching happens here on the main thread, so Lua callbacks
        // never race with the recv thread that fills the queue.
        m_osc.poll(m_osc_msgs);
        for (const auto& msg : m_osc_msgs) {
            // ── Engine-level OSC: /scene <path> ──────────────────────────────
            // Intercepted before Lua dispatch so it works regardless of whether
            // the current scene defines on_osc, or even if it has crashed.
            // First string argument is the scene file path, e.g.:
            //   /scene "scenes/matrix.lua"
            // Path is relative to the working directory (same as -s at startup).
            if (msg.address == "/scene") {
                if (!msg.args.empty() && msg.args[0].type == 's') {
                    const std::string& path = msg.args[0].s;
                    // Reject path traversal sequences.  A path containing ".."
                    // as a component could escape the working directory and load
                    // an arbitrary file from the filesystem.  Since all valid
                    // scene paths are relative (e.g. "scenes/foo.lua"), any
                    // occurrence of ".." is considered hostile and discarded.
                    if (path.find("..") != std::string::npos) {
                        fprintf(stderr,
                            "OSC /scene: rejected path '%s' (contains '..')\n",
                            path.c_str());
                    } else {
                        load_scene(path.c_str());
                    }
                }
                continue;   // do not forward to Lua's on_osc
            }

            // Look up the global function on_osc.  If it isn't defined in the
            // current scene, silently skip — not every scene needs OSC input.
            if (lua_getglobal(m_lua.L, "on_osc") != LUA_TFUNCTION) {
                lua_pop(m_lua.L, 1);
                continue;
            }

            // Push address then each argument in order.
            // OSC int/float → Lua number, OSC string → Lua string.
            // Other types (blob, timetag, MIDI) were already filtered out by
            // the parser in osc.cpp, so we only see 'i', 'f', 's' here.
            lua_pushstring(m_lua.L, msg.address.c_str());
            for (const auto& arg : msg.args) {
                if      (arg.type == 'i') lua_pushinteger(m_lua.L, arg.i);
                else if (arg.type == 'f') lua_pushnumber(m_lua.L,  arg.f);
                else if (arg.type == 's') lua_pushstring(m_lua.L,  arg.s.c_str());
            }

            int nargs = 1 + (int)msg.args.size();
            if (lua_pcall(m_lua.L, nargs, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(m_lua.L, -1);
                fprintf(stderr, "Lua error [on_osc %s]: %s\n",
                        msg.address.c_str(), err ? err : "(no message)");
                lua_pop(m_lua.L, 1);
            }
        }

        // ── Hot reload: poll scene file mtime ─────────────────────────────
        // stat() is cheap (a single syscall) so calling it every frame is fine.
        // We use a 150 ms debounce so editors that write files in multiple
        // steps (save → truncate → write) don't trigger a mid-write reload.
        if (!m_scene_path.empty()) {
            time_t new_mtime = file_mtime(m_scene_path.c_str());
            if (new_mtime != 0 && new_mtime != m_scene_mtime) {
                if (!m_reload_pending) {
                    // First frame where we noticed a change — start the timer.
                    m_reload_pending = true;
                    m_reload_timer   = now;
                } else if (now - m_reload_timer >= 150) {
                    // Debounce elapsed — safe to reload.
                    reload_scene();
                }
            }
        }

        handle_events();

        // Reset renderer's CPU vertex buffer and transform stack for this frame.
        if (m_has_scene) m_renderer.begin_frame();

        // Call the Lua on_frame(dt) hook.  Scripts call clear(), draw_rect(),
        // etc. — these accumulate into the renderer's vertex buffer.
        m_lua.call_hook("on_frame", (double)dt);

        // Flush vertices, run post-process pipeline, blit to screen,
        // copy result to feedback FBO.
        if (m_has_scene) m_renderer.end_frame(&m_pipeline, m_time, 0.0f);

        // Fallback triangle — only when no scene is loaded.
        if (!m_has_scene) render_fallback();

        // Swap the back buffer to the screen (respects vsync interval set above).
        SDL_GL_SwapWindow(m_window);
    }
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

// ── render() ─────────────────────────────────────────────────────────────────

void Engine::render_fallback() {
    // Clear to a dark background so the triangle stands out.
    glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw the demo triangle.
    glUseProgram(m_shader_prog);
    glBindVertexArray(m_vao);
    // 3 vertices, starting at index 0, using the triangle primitive type.
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
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

    // SDL_PumpEvents forces SDL to collect pending OS events (including the
    // window resize notification from the fullscreen transition) before we
    // query the new drawable size.  Without this, SDL_GL_GetDrawableSize can
    // return stale dimensions on macOS because the OS resize hasn't been
    // acknowledged yet.
    SDL_PumpEvents();

    // Update the viewport, renderer FBOs, and Lua screen globals together.
    // SDL_WINDOWEVENT_SIZE_CHANGED may also fire and call these again — that's
    // harmless since set_size / set_screen_size are idempotent with equal values.
    SDL_GL_GetDrawableSize(m_window, &m_draw_w, &m_draw_h);
    glViewport(0, 0, m_draw_w, m_draw_h);
    m_renderer.set_size(m_draw_w, m_draw_h);
    m_lua.set_screen_size(m_draw_w, m_draw_h);
}

// ── compile_shader() ─────────────────────────────────────────────────────────

unsigned int Engine::compile_shader(unsigned int type, const char* src) {
    // Create a shader object on the GPU and upload the GLSL source text.
    unsigned int id = glCreateShader(type);
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);

    // Check if compilation succeeded.
    int ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        // Fetch and print the driver's error log — it contains the line number
        // and description, e.g. "0(12) : error C0000: syntax error".
        char log[512];
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compile error (%s):\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

// ── link_program() ───────────────────────────────────────────────────────────

unsigned int Engine::link_program(unsigned int vert, unsigned int frag) {
    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    int ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "Program link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }

    // The individual shader objects are no longer needed once linked into
    // a program — mark them for deletion (they're freed when detached).
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

// ── setup_triangle() ─────────────────────────────────────────────────────────

void Engine::setup_triangle() {
    // Compile and link the demo shaders.
    unsigned int vert = compile_shader(GL_VERTEX_SHADER,   k_vert_src);
    unsigned int frag = compile_shader(GL_FRAGMENT_SHADER, k_frag_src);
    if (!vert || !frag) return;
    m_shader_prog = link_program(vert, frag);
    if (!m_shader_prog) return;

    // A VAO (Vertex Array Object) records the vertex format description so we
    // don't have to re-specify it every frame — binding the VAO is enough.
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    // A VBO (Vertex Buffer Object) is a chunk of GPU memory holding our
    // vertex data.  GL_STATIC_DRAW tells the driver this data won't change,
    // so it can live in fast video memory.
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_triangle), k_triangle, GL_STATIC_DRAW);

    // Tell the GPU how to interpret the raw bytes in the VBO.
    // Each vertex is 5 floats: [x, y, r, g, b]
    // Stride = 5 * sizeof(float) — distance in bytes between consecutive vertices.

    // Attribute 0 — position: 2 floats at byte offset 0.
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Attribute 1 — colour: 3 floats at byte offset 2*sizeof(float).
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                          5 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Unbind the VAO — we'll rebind it in render() when we want to draw.
    glBindVertexArray(0);
}
