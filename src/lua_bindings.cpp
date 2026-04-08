// glad.h first — must precede any other GL or SDL headers.
#include <glad/glad.h>

#include "lua_bindings.h"
#include "lua_automata.h"
#include "lua_noise.h"
#include "lua_image.h"
#include "lua_canvas.h"
#include "lua_waveform.h"
#include "lua_3d.h"
#include "lua_text.h"
#include "renderer.h"
#include "shader_pipeline.h"
#include <vector>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ── Registry keys ─────────────────────────────────────────────────────────────
// We use the addresses of static chars as unique registry keys.
// The address of a static variable is guaranteed unique per process —
// no string comparison or naming collision is possible.
static const char k_renderer_key = '\0';
static const char k_pipeline_key = '\0';

// Retrieve the Renderer pointer stored by set_renderer().
// Public (declared in lua_bindings.h) so other modules can issue draw calls.
Renderer* lua_bindings::get_renderer(lua_State* L) {
    lua_pushlightuserdata(L, (void*)&k_renderer_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    Renderer* r = (Renderer*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return r;
}

// Internal alias for use within this file — avoids prefixing every call.
static Renderer* get_renderer(lua_State* L) {
    return lua_bindings::get_renderer(L);
}

// Retrieve the ShaderPipeline pointer stored by set_pipeline().
static ShaderPipeline* get_pipeline(lua_State* L) {
    lua_pushlightuserdata(L, (void*)&k_pipeline_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    ShaderPipeline* p = (ShaderPipeline*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return p;
}

// ── Binding: clear(r, g, b, a) ────────────────────────────────────────────────
// Lua owns the frame when a scene is loaded — this is its way to clear the
// colour buffer.  All arguments optional; defaults to opaque black.
static int l_clear(lua_State* L) {
    float r = (float)luaL_optnumber(L, 1, 0.0);
    float g = (float)luaL_optnumber(L, 2, 0.0);
    float b = (float)luaL_optnumber(L, 3, 0.0);
    float a = (float)luaL_optnumber(L, 4, 1.0);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    return 0;
}

// ── Transform stack ───────────────────────────────────────────────────────────

static int l_push(lua_State* L) {
    get_renderer(L)->push();
    return 0;
}

static int l_pop(lua_State* L) {
    get_renderer(L)->pop();
    return 0;
}

static int l_translate(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    get_renderer(L)->translate(x, y);
    return 0;
}

static int l_rotate(lua_State* L) {
    // Angle in radians — same as Processing.
    float a = (float)luaL_checknumber(L, 1);
    get_renderer(L)->rotate(a);
    return 0;
}

static int l_scale(lua_State* L) {
    float sx = (float)luaL_checknumber(L, 1);
    // If only one arg, scale uniformly.
    float sy = (float)luaL_optnumber(L, 2, (double)sx);
    get_renderer(L)->scale(sx, sy);
    return 0;
}

// ── Colour state ──────────────────────────────────────────────────────────────

static int l_set_color(lua_State* L) {
    float r = (float)luaL_checknumber(L, 1);
    float g = (float)luaL_checknumber(L, 2);
    float b = (float)luaL_checknumber(L, 3);
    float a = (float)luaL_optnumber(L, 4, 1.0);
    get_renderer(L)->set_color(r, g, b, a);
    return 0;
}

static int l_set_stroke(lua_State* L) {
    float r = (float)luaL_checknumber(L, 1);
    float g = (float)luaL_checknumber(L, 2);
    float b = (float)luaL_checknumber(L, 3);
    float a = (float)luaL_optnumber(L, 4, 1.0);
    get_renderer(L)->set_stroke(r, g, b, a);
    return 0;
}

static int l_set_stroke_weight(lua_State* L) {
    float w = (float)luaL_checknumber(L, 1);
    get_renderer(L)->set_stroke_weight(w);
    return 0;
}

static int l_set_circle_segments(lua_State* L) {
    int n = (int)luaL_checkinteger(L, 1);
    get_renderer(L)->set_circle_segments(n);
    return 0;
}

// ── Drawing primitives ────────────────────────────────────────────────────────

static int l_draw_rect(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float w = (float)luaL_checknumber(L, 3);
    float h = (float)luaL_checknumber(L, 4);
    get_renderer(L)->draw_rect(x, y, w, h);
    return 0;
}

static int l_draw_circle(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float r = (float)luaL_checknumber(L, 3);
    get_renderer(L)->draw_circle(x, y, r);
    return 0;
}

static int l_draw_line(lua_State* L) {
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    get_renderer(L)->draw_line(x1, y1, x2, y2);
    return 0;
}

static int l_draw_point(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    get_renderer(L)->draw_point(x, y);
    return 0;
}

// ── Feedback ──────────────────────────────────────────────────────────────────

// draw_feedback(alpha, scale, angle)
// Blits the previous frame's final image into the current scene FBO.
// All arguments are optional:
//   alpha — blend weight (default 1.0)
//   scale — quad scale around screen centre (default 1.0)
//   angle — rotation in radians (default 0.0)
static int l_draw_feedback(lua_State* L) {
    float alpha = (float)luaL_optnumber(L, 1, 1.0);
    float scale = (float)luaL_optnumber(L, 2, 1.0);
    float angle = (float)luaL_optnumber(L, 3, 0.0);
    get_renderer(L)->draw_feedback(alpha, scale, angle);
    return 0;
}

// ── Shader pipeline control ───────────────────────────────────────────────────

// shader_set("name1", "name2", ...)
// Replaces the entire post-process pipeline with the named shaders.
// Each name maps to shaders/<name>.frag.
static int l_shader_set(lua_State* L) {
    std::vector<std::string> names;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        names.push_back(luaL_checkstring(L, i));
    }
    get_pipeline(L)->set(names);
    return 0;
}

// shader_add("name")
// Append a single shader to the end of the pipeline.
static int l_shader_add(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    get_pipeline(L)->add(name);
    return 0;
}

// shader_clear()
// Remove all post-process shaders (pipeline becomes passthrough).
static int l_shader_clear(lua_State* L) {
    (void)L;
    get_pipeline(L)->clear();
    return 0;
}

// shader_set_uniform("name", value)
// Set a float uniform on all shaders in the current pipeline.
static int l_shader_set_uniform(lua_State* L) {
    const char* name  = luaL_checkstring(L, 1);
    float       value = (float)luaL_checknumber(L, 2);
    get_pipeline(L)->set_uniform(name, value);
    return 0;
}

// ── register_all() ────────────────────────────────────────────────────────────

void lua_bindings::set_renderer(lua_State* L, Renderer* r) {
    lua_pushlightuserdata(L, (void*)&k_renderer_key);
    lua_pushlightuserdata(L, (void*)r);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

void lua_bindings::set_pipeline(lua_State* L, ShaderPipeline* p) {
    lua_pushlightuserdata(L, (void*)&k_pipeline_key);
    lua_pushlightuserdata(L, (void*)p);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

void lua_bindings::register_all(lua_State* L) {
    // Clear / frame control
    lua_register(L, "clear",               l_clear);

    // Transform stack
    lua_register(L, "push",                l_push);
    lua_register(L, "pop",                 l_pop);
    lua_register(L, "translate",           l_translate);
    lua_register(L, "rotate",              l_rotate);
    lua_register(L, "scale",               l_scale);

    // Colour state
    lua_register(L, "set_color",           l_set_color);
    lua_register(L, "set_stroke",          l_set_stroke);
    lua_register(L, "set_stroke_weight",   l_set_stroke_weight);
    lua_register(L, "set_circle_segments", l_set_circle_segments);

    // Drawing primitives
    lua_register(L, "draw_rect",           l_draw_rect);
    lua_register(L, "draw_circle",         l_draw_circle);
    lua_register(L, "draw_line",           l_draw_line);
    lua_register(L, "draw_point",          l_draw_point);

    // Feedback + post-process pipeline
    lua_register(L, "draw_feedback",       l_draw_feedback);
    lua_register(L, "shader_set",          l_shader_set);
    lua_register(L, "shader_add",          l_shader_add);
    lua_register(L, "shader_clear",        l_shader_clear);
    lua_register(L, "shader_set_uniform",  l_shader_set_uniform);

    // Cellular automata (wolfram and conway global tables)
    lua_automata::register_all(L);

    // Noise functions (noise, fbm globals)
    lua_noise::register_all(L);

    // Image loading (image and sprite_sheet global tables)
    lua_image::register_all(L);

    // Offscreen canvas (canvas global table)
    lua_canvas::register_all(L);

    // Waveform generators (wave_sine, wave_saw, wave_square, wave_tri, draw_waveform globals)
    lua_waveform::register_all(L);

    // 3D wireframe (camera_3d, perspective_3d, project_3d, draw_wire_cube, etc. globals)
    lua_3d::register_all(L);

    // Bitmap text (draw_text, text_width globals)
    lua_text::register_all(L);
}
