// glad.h first — must precede any other GL or SDL headers.
#include <glad/glad.h>

#include "lua_bindings.h"
#include "lua_easing.h"
#include "lua_automata.h"
#include "lua_noise.h"
#include "lua_image.h"
#include "lua_canvas.h"
#include "lua_waveform.h"
#include "lua_3d.h"
#include "lua_text.h"
#include "lua_vec.h"
#include "lua_params.h"
#include "lua_clock.h"
#include "lua_slew.h"
#include "snapshot.h"
#include "renderer.h"
#include "shader_pipeline.h"
#include <vector>
#include <string>
#include <cstring>   // strcmp for set_blend
#include <cmath>     // fmodf for the HSV conversion

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

    // Commit any geometry queued before this call BEFORE clearing.
    //
    // Draw calls don't reach the GPU immediately — they accumulate in the
    // renderer's CPU vertex buffer and are flushed later.  Without this flush,
    // shapes drawn before clear() would still be sitting in that buffer when
    // glClear wipes the framebuffer, and would then be drawn on top of the
    // fresh background — surviving the very clear that should have erased them.
    // Flushing first makes clear() mean what it says at any point in a frame.
    Renderer* rend = get_renderer(L);
    if (rend) rend->flush();

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


// ── HSV colour ────────────────────────────────────────────────────────────────
//
// RGB is how the hardware thinks; HSV is how visuals think. "Rotate the hue
// over time" is the single most common colour gesture in this idiom, and in RGB
// it is an awkward three-channel dance — in HSV it is one number going up.
//
// Convention here matches everything else in the engine: all components are
// 0..1 floats, not degrees. Hue wraps, so h = 1.25 and h = 0.25 are the same
// colour and you can feed it an ever-increasing number without wrapping it
// yourself.
static void hsv_to_rgb(float h, float s, float v,
                       float& r, float& g, float& b)
{
    // Wrap hue into [0,1). fmodf keeps the sign of its argument, so a negative
    // hue needs the extra shift.
    h = fmodf(h, 1.0f);
    if (h < 0.0f) h += 1.0f;

    if (s <= 0.0f) { r = g = b = v; return; }   // grey: hue is meaningless
    if (s > 1.0f) s = 1.0f;

    // Six 60-degree sectors around the colour wheel. Within a sector two
    // channels are fixed (at v and at the darkest value) and the third ramps.
    const float sector = h * 6.0f;
    const int   i      = (int)sector;
    const float f      = sector - (float)i;     // position within the sector

    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));

    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;   // red    -> yellow
        case 1: r = q; g = v; b = p; break;   // yellow -> green
        case 2: r = p; g = v; b = t; break;   // green  -> cyan
        case 3: r = p; g = q; b = v; break;   // cyan   -> blue
        case 4: r = t; g = p; b = v; break;   // blue   -> magenta
        default: r = v; g = p; b = q; break;  // magenta-> red
    }
}

// hsv(h, s, v [, a]) -> r, g, b, a
//
// Returns four values rather than setting anything, so it composes with every
// call that already takes a colour:
//
//     set_color(hsv(elapsed() * 0.1, 0.9, 1))
//     clear(hsv(0.6, 0.4, 0.15))
//
// (Four return values expand only in the last argument position — which is
// where they are here, as the whole argument list.)
static int l_hsv(lua_State* L) {
    float h = (float)luaL_checknumber(L, 1);
    float s = (float)luaL_checknumber(L, 2);
    float v = (float)luaL_checknumber(L, 3);
    float a = (float)luaL_optnumber(L, 4, 1.0);

    float r, g, b;
    hsv_to_rgb(h, s, v, r, g, b);
    lua_pushnumber(L, r);
    lua_pushnumber(L, g);
    lua_pushnumber(L, b);
    lua_pushnumber(L, a);
    return 4;
}

// set_color_hsv(h, s, v [, a]) — shorthand for set_color(hsv(...)).
static int l_set_color_hsv(lua_State* L) {
    float h = (float)luaL_checknumber(L, 1);
    float s = (float)luaL_checknumber(L, 2);
    float v = (float)luaL_checknumber(L, 3);
    float a = (float)luaL_optnumber(L, 4, 1.0);
    float r, g, b;
    hsv_to_rgb(h, s, v, r, g, b);
    Renderer* rend = get_renderer(L);
    if (rend) rend->set_color(r, g, b, a);
    return 0;
}

// set_stroke_hsv(h, s, v [, a])
static int l_set_stroke_hsv(lua_State* L) {
    float h = (float)luaL_checknumber(L, 1);
    float s = (float)luaL_checknumber(L, 2);
    float v = (float)luaL_checknumber(L, 3);
    float a = (float)luaL_optnumber(L, 4, 1.0);
    float r, g, b;
    hsv_to_rgb(h, s, v, r, g, b);
    Renderer* rend = get_renderer(L);
    if (rend) rend->set_stroke(r, g, b, a);
    return 0;
}

// set_blend("alpha" | "add" | "multiply" | "screen")
// Controls how subsequent draws combine with what's already on screen.
// Resets to "alpha" at the start of every frame.
//
//   alpha    — normal layering (default)
//   add      — light accumulates; overlapping strokes brighten toward white.
//              This is the phosphor/neon look, especially with draw_feedback.
//   multiply — darkens; useful for shadows and tinting
//   screen   — lightens like add, but saturates gently instead of clipping
static int l_set_blend(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    BlendMode mode;
    if      (strcmp(name, "alpha")    == 0) mode = BlendMode::ALPHA;
    else if (strcmp(name, "add")      == 0) mode = BlendMode::ADD;
    else if (strcmp(name, "multiply") == 0) mode = BlendMode::MULTIPLY;
    else if (strcmp(name, "screen")   == 0) mode = BlendMode::SCREEN;
    else return luaL_error(L, "set_blend: unknown mode '%s' "
                              "(use 'alpha', 'add', 'multiply' or 'screen')", name);

    Renderer* r = get_renderer(L);
    if (r) r->set_blend(mode);
    return 0;
}

// elapsed() → seconds since startup
// The same clock the engine feeds to shaders as u_time, so Lua animation and
// shader animation stay in step. Affected by pause and the time-scale keys,
// which is the point: scenes that use it can be slowed down or held live,
// whereas a scene accumulating its own `t = t + dt` only follows dt.
static int l_elapsed(lua_State* L) {
    Renderer* r = get_renderer(L);
    lua_pushnumber(L, r ? (double)r->get_time() : 0.0);
    return 1;
}

// snapshot([name]) → save this frame as a PNG under snapshots/
//
// With no argument the file is timestamped; with a string it becomes the
// filename stem. Call it from on_frame or on_beat to have a scene capture
// itself — on a downbeat, at the end of an animation, when a value crosses a
// threshold — rather than needing a hand on the keyboard.
//
// Note the ordering: on_frame runs BEFORE the renderer composites, so the
// request is queued and serviced after this frame is finished. You always get
// the frame you were drawing when you asked, never the one before it.
//
// Calling it every frame will produce a file every frame, which will fill the
// disk and eventually make the writer thread the slowest thing in the process.
// Gate it on something.
static int l_snapshot(lua_State* L) {
    const char* name = luaL_optstring(L, 1, nullptr);
    snapshot::request(name);
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

// Read a uniform's components from the Lua stack starting at `idx`.
//
// Accepts 1-4 numbers, or a vec (which counts as two). Shared by the global
// shader_set_uniform and canvas:set_uniform so the two cannot drift apart.
int lua_bindings::read_uniform_args(lua_State* L, int idx, float* out) {
    float vx, vy;
    if (lua_vec::get(L, idx, vx, vy)) {
        out[0] = vx;
        out[1] = vy;
        return 2;
    }
    int n = 0;
    while (n < 4 && !lua_isnoneornil(L, idx + n)) {
        out[n] = (float)luaL_checknumber(L, idx + n);
        n++;
    }
    if (n == 0) luaL_error(L, "set_uniform: needs a value");
    return n;
}

// Read an array of numbers from a Lua table at `idx` into `out`.
int lua_bindings::read_data_table(lua_State* L, int idx, std::vector<float>& out) {
    luaL_checktype(L, idx, LUA_TTABLE);
    const lua_Integer n = luaL_len(L, idx);
    out.clear();
    out.reserve((size_t)n);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, idx, i);
        out.push_back((float)lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return (int)out.size();
}

// shader_set_uniform("name", value [, y [, z [, w]]])
// shader_set_uniform("name", vec)
//
// Sets a float, vec2, vec3 or vec4 uniform on every shader in the pipeline
// that declares one by that name.
static int l_shader_set_uniform(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    float v[4];
    const int n = lua_bindings::read_uniform_args(L, 2, v);
    get_pipeline(L)->set_uniform(name, v, n);
    return 0;
}

// shader_set_data("name", table)
//
// Uploads an array of numbers as a 1-D texture the shaders can index per pixel:
//
//     uniform sampler2D u_bands;
//     float band = texture(u_bands, vec2(v_uv.x, 0.5)).r;
//
// A uniform can carry four numbers; a spectrum or a per-element curve is
// hundreds, and only a texture can be sampled per fragment.
static int l_shader_set_data(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::vector<float> values;
    const int n = lua_bindings::read_data_table(L, 2, values);
    if (n > 0) get_pipeline(L)->set_data(name, values.data(), n);
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
    lua_register(L, "set_blend",           l_set_blend);
    lua_register(L, "hsv",                 l_hsv);
    lua_register(L, "set_color_hsv",       l_set_color_hsv);
    lua_register(L, "set_stroke_hsv",      l_set_stroke_hsv);

    // Engine clock
    lua_register(L, "elapsed",             l_elapsed);

    // Frame capture
    lua_register(L, "snapshot",            l_snapshot);

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
    lua_register(L, "shader_set_data",     l_shader_set_data);

    // Easing and utility math (lerp, smooth, smooth_hl, map, clamp, smoothstep, pulse)
    lua_easing::register_all(L);

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

    // 2D vector type (vec global)
    lua_vec::register_all(L);

    // OSC routing and parameters (on, param globals)
    lua_params::register_all(L);

    // Musical clock (bpm, beat_phase, bar_phase, env globals)
    lua_clock::register_all(L);

    // Self-smoothing named variables (slew global)
    lua_slew::register_all(L);
}
