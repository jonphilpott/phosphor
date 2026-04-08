// lua_canvas.cpp
// Offscreen render-target (canvas) exposed to Lua.
//
// A canvas is an FBO pair + a local ShaderPipeline.  Lua renders into it with
// canvas:begin() / canvas:finish(), then composites it into the scene with
// canvas:draw().  Each canvas carries its own shader chain so effects can be
// scoped to a region without affecting the rest of the screen.
//
// Lifecycle:
//   local c = canvas.new(400, 300)   -- allocate FBOs once (in on_load or at top level)
//   function on_frame(dt)
//       c:begin()
//           clear(0,0,0,1)
//           draw_rect(...)
//       c:finish("scanlines")        -- apply shader(s) only to this canvas
//       c:draw(x, y)                 -- composite into main scene
//   end

#include "lua_canvas.h"
#include "lua_bindings.h"       // lua_bindings::get_renderer
#include "renderer.h"
#include "shader_pipeline.h"

#include <glad/glad.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static const char* CANVAS_MT = "phosphor.canvas";

// ── Canvas struct ─────────────────────────────────────────────────────────────
//
// Two FBOs are kept per canvas:
//   fbo[0] / fbo_tex[0]  — primary render target (canvas:begin() binds this)
//   fbo[1] / fbo_tex[1]  — ping-pong partner for local shader chain
//
// After canvas:finish(), `result` holds the index (0 or 1) of the FBO whose
// texture contains the final composited image.  canvas:draw() reads from
// fbo_tex[result].
//
// ShaderPipeline is a non-trivial C++ object so it cannot be embedded directly
// in a C struct that Lua manages.  We heap-allocate it and store a pointer.

struct Canvas {
    int           width, height;
    unsigned int  fbo[2];
    unsigned int  fbo_tex[2];
    int           result;         // 0 or 1 — which fbo has the current frame
    ShaderPipeline* pipeline;     // owned; new'd in l_canvas_new, delete'd in __gc
};

static Canvas* check_canvas(lua_State* L) {
    return (Canvas*)luaL_checkudata(L, 1, CANVAS_MT);
}

// ── FBO helpers ───────────────────────────────────────────────────────────────

// Allocate one FBO with a colour texture attachment.
// Returns false if the FBO is incomplete (out of memory, driver bug, etc.).
static bool make_fbo(int w, int h, unsigned int& fbo, unsigned int& tex) {
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

static void delete_fbo(unsigned int& fbo, unsigned int& tex) {
    if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
    if (tex) { glDeleteTextures(1, &tex);      tex = 0; }
}

// ── Lua methods ───────────────────────────────────────────────────────────────

// canvas:begin()
// Flushes pending geometry, then redirects all subsequent draw calls into this
// canvas's FBO until canvas:finish() is called.
// The render-target stack in Renderer handles save/restore automatically.
static int l_canvas_begin(lua_State* L) {
    Canvas*   c = check_canvas(L);
    Renderer* r = lua_bindings::get_renderer(L);
    if (!r) return 0;

    // Reset to fbo[0] as the primary render target for this canvas.
    // (fbo[1] is used only during shader ping-pong in finish().)
    c->result = 0;
    r->push_target(c->fbo[0], c->width, c->height);
    return 0;
}

// canvas:finish([shader_name, ...])
// Flushes geometry, runs the optional local shader chain within the canvas FBOs,
// then restores the previous render target (typically the main scene FBO).
//
// Zero or more shader names can be passed — same names as shader_set() accepts.
// Example: c:finish("scanlines", "chromatic_ab")
static int l_canvas_finish(lua_State* L) {
    Canvas*   c = check_canvas(L);
    Renderer* r = lua_bindings::get_renderer(L);
    if (!r) return 0;

    // Collect optional shader names from varargs (args 2..n).
    int n = lua_gettop(L);
    std::vector<std::string> names;
    for (int i = 2; i <= n; ++i)
        names.push_back(luaL_checkstring(L, i));

    // Step 1: Flush canvas geometry into fbo[0] while it is still bound.
    // This must happen before apply() — apply() internally calls glBindFramebuffer
    // to set up its own ping-pong, so any un-flushed geometry would end up in the
    // wrong FBO if we don't commit it here first.
    r->flush();

    // Step 2: Run the local shader chain if shaders were requested.
    // apply() explicitly binds the FBOs it needs (it does not use the currently
    // bound framebuffer as an implicit source).  After it returns, the GL
    // framebuffer is set to whichever canvas FBO holds the result.
    if (!names.empty()) {
        c->pipeline->set(names);
        c->result = c->pipeline->apply(c->fbo, c->fbo_tex,
                                       c->width, c->height,
                                       r->get_time(), 0.0f);
    }
    // If no shaders, c->result stays 0 (fbo[0] holds the raw render).

    // Step 3: Pop the render-target stack.
    // pop_target() calls flush_verts() (no-op — we just flushed) then explicitly
    // rebinds the FBO that was on top of the stack before begin() pushed ours.
    // This correctly undoes the apply() FBO side-effect: the main scene FBO is
    // restored regardless of which canvas FBO apply() left bound.
    r->pop_target();

    return 0;
}

// canvas:draw(x, y [, w, h [, angle]])
// Blits the canvas result texture into the current render target using draw_image().
// w and h default to the canvas pixel dimensions (1:1 size).
// angle (optional, radians) rotates the canvas around its own centre.
//   Positive = counter-clockwise.  Default 0.
//
// NOTE: canvas:draw() uses draw_image() which bypasses the CPU transform matrix.
// push/translate/rotate/scale do NOT affect canvas draws — use the angle
// parameter here instead.
static int l_canvas_draw(lua_State* L) {
    Canvas*   c     = check_canvas(L);
    float     x     = (float)luaL_checknumber(L, 2);
    float     y     = (float)luaL_checknumber(L, 3);
    float     w     = (float)luaL_optnumber(L, 4, (double)c->width);
    float     h     = (float)luaL_optnumber(L, 5, (double)c->height);
    float     angle = (float)luaL_optnumber(L, 6, 0.0);
    Renderer* r     = lua_bindings::get_renderer(L);
    if (!r) return 0;

    // The canvas FBO texture has OpenGL convention (v=0 at bottom) which matches
    // the stb_image-flipped convention used by draw_image — both have v=1 at the
    // visual top — so full UV range (0,0,1,1) draws the canvas right-side up.
    r->draw_image(c->fbo_tex[c->result], x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, angle);
    return 0;
}

// canvas:set_uniform(name, value)
// Sets a float uniform on this canvas's local shader pipeline.
// The value is stored in the pipeline's uniform map and uploaded to every
// shader in the chain during the next canvas:finish() call.
//
// This is the canvas-local equivalent of the global shader_set_uniform().
// It only affects THIS canvas's pipeline — not the main post-process chain.
//
// Uniforms persist across frames until overwritten, so it's safe to set them
// once in on_load() if they never change, or every frame if they animate.
//
// Example — render a Julia set into a canvas with custom c and zoom:
//   c:begin()
//     clear(0,0,0,1)
//   c:set_uniform("u_julia_cx", -0.75)
//   c:set_uniform("u_julia_cy",  0.11)
//   c:set_uniform("u_zoom",      1.5)
//   c:set_uniform("u_max_iter",  128)
//   c:finish("julia")
static int l_canvas_set_uniform(lua_State* L) {
    Canvas*     c     = check_canvas(L);
    const char* name  = luaL_checkstring(L, 2);
    float       value = (float)luaL_checknumber(L, 3);
    if (c->pipeline)
        c->pipeline->set_uniform(name, value);
    return 0;
}

static int l_canvas_width(lua_State* L) {
    lua_pushinteger(L, check_canvas(L)->width);
    return 1;
}

static int l_canvas_height(lua_State* L) {
    lua_pushinteger(L, check_canvas(L)->height);
    return 1;
}

// __gc: called by the Lua GC when the canvas is no longer reachable.
// Frees both FBOs and the heap-allocated ShaderPipeline.
static int l_canvas_gc(lua_State* L) {
    Canvas* c = (Canvas*)luaL_checkudata(L, 1, CANVAS_MT);
    delete_fbo(c->fbo[0], c->fbo_tex[0]);
    delete_fbo(c->fbo[1], c->fbo_tex[1]);
    if (c->pipeline) {
        c->pipeline->shutdown();
        delete c->pipeline;
        c->pipeline = nullptr;
    }
    return 0;
}

static int l_canvas_tostring(lua_State* L) {
    Canvas* c = check_canvas(L);
    lua_pushfstring(L, "canvas(%d x %d)", c->width, c->height);
    return 1;
}

// canvas.new(w, h) → Canvas userdata
static int l_canvas_new(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    luaL_argcheck(L, w >= 1, 1, "width must be >= 1");
    luaL_argcheck(L, h >= 1, 2, "height must be >= 1");

    Canvas* c = (Canvas*)lua_newuserdata(L, sizeof(Canvas));
    memset(c, 0, sizeof(Canvas));
    c->width  = w;
    c->height = h;

    // Set metatable early so __gc runs even if the subsequent allocations fail.
    luaL_setmetatable(L, CANVAS_MT);

    // Allocate both FBOs.
    if (!make_fbo(w, h, c->fbo[0], c->fbo_tex[0]) ||
        !make_fbo(w, h, c->fbo[1], c->fbo_tex[1])) {
        luaL_error(L, "canvas.new: failed to create FBO (%dx%d)", w, h);
    }

    // Heap-allocate and initialise the local shader pipeline.
    // ShaderPipeline::init() compiles the shared fullscreen-quad vertex shader
    // and sets up the VAO/VBO for post-process passes.
    c->pipeline = new ShaderPipeline();
    if (!c->pipeline->init()) {
        delete c->pipeline;
        c->pipeline = nullptr;
        luaL_error(L, "canvas.new: failed to init shader pipeline");
    }

    return 1;
}

// ── Registration ──────────────────────────────────────────────────────────────

void lua_canvas::register_all(lua_State* L) {
    static const luaL_Reg canvas_methods[] = {
        { "begin",       l_canvas_begin       },
        { "finish",      l_canvas_finish      },
        { "draw",        l_canvas_draw        },
        { "set_uniform", l_canvas_set_uniform },
        { "width",       l_canvas_width       },
        { "height",      l_canvas_height      },
        { nullptr, nullptr }
    };

    luaL_newmetatable(L, CANVAS_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, canvas_methods, 0);
    lua_pushcfunction(L, l_canvas_gc);       lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_canvas_tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_canvas_new);
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "canvas");
}
