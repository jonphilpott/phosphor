// lua_waveform.cpp
// Waveform generator functions for Lua scenes.
//
// Two layers:
//
//   Value functions — return a float in [-1, 1] for a given phase t.
//   t is in cycles (0 = start, 1 = one full period, 2 = two periods, etc.).
//   These are useful as modulators: drive position, colour, size, speed, etc.
//
//     wave_sine(t)
//     wave_saw(t)
//     wave_square(t [, duty])   -- duty 0..1, default 0.5
//     wave_tri(t)
//
//   Renderer function — draws a waveform as a polyline into the current scene.
//   Uses the current stroke colour and stroke weight.
//   Respects the CPU transform matrix (push/translate/rotate/scale apply).
//
//     draw_waveform(type, x, y, w, h [, cycles [, phase]])
//       type   — "sine", "saw", "square", "tri"
//       x, y   — top-left of the bounding rectangle in pixel space
//       w, h   — dimensions of the bounding rectangle
//       cycles — number of complete waveform cycles across the width (default 1)
//       phase  — starting phase in cycles (default 0)
//
// Example usage:
//
//   -- Modulate circle radius with a triangle wave
//   local r = (wave_tri(t * 2) * 0.5 + 0.5) * 100 + 20
//   draw_circle(cx, cy, r)
//
//   -- Draw a dual-channel oscilloscope
//   set_stroke(0, 1, 0.4, 1) set_stroke_weight(1.5)
//   draw_waveform("sine", 0, 100, screen_width, 150, 3)
//   set_stroke(1, 0.4, 0.1, 1)
//   draw_waveform("saw",  0, 300, screen_width, 150, 3, t)  -- scrolling phase

#include "lua_waveform.h"
#include "lua_bindings.h"
#include "renderer.h"

#include <cmath>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ── Waveform math ─────────────────────────────────────────────────────────────
// All functions take t in cycles and return a value in [-1, 1].
// fmod is not used for the fractional part; instead we use t - floorf(t)
// which is equivalent but avoids the negative-modulo ambiguity.

static inline float w_sine(float t) {
    return sinf(t * 6.28318530f);
}

static inline float w_saw(float t) {
    // Ramps linearly from -1 (t=0) to +1 (t→1), then jumps back.
    float frac = t - floorf(t);   // fractional part in [0, 1)
    return 2.0f * frac - 1.0f;
}

static inline float w_square(float t, float duty) {
    // High (+1) for the first `duty` fraction of the period, then low (-1).
    float frac = t - floorf(t);
    return (frac < duty) ? 1.0f : -1.0f;
}

static inline float w_tri(float t) {
    // Rises from -1 to +1 in the first half-period, falls back in the second.
    float frac = t - floorf(t);   // [0, 1)
    return (frac < 0.5f) ? (4.0f * frac - 1.0f) : (3.0f - 4.0f * frac);
}

// ── Lua value functions ───────────────────────────────────────────────────────

static int l_wave_sine(lua_State* L) {
    float t = (float)luaL_checknumber(L, 1);
    lua_pushnumber(L, w_sine(t));
    return 1;
}

static int l_wave_saw(lua_State* L) {
    float t = (float)luaL_checknumber(L, 1);
    lua_pushnumber(L, w_saw(t));
    return 1;

}

static int l_wave_square(lua_State* L) {
    float t    = (float)luaL_checknumber(L, 1);
    float duty = (float)luaL_optnumber(L, 2, 0.5);
    // Clamp duty to a sensible range so the waveform doesn't degenerate.
    if (duty < 0.01f) duty = 0.01f;
    if (duty > 0.99f) duty = 0.99f;
    lua_pushnumber(L, w_square(t, duty));
    return 1;
}

static int l_wave_tri(lua_State* L) {
    float t = (float)luaL_checknumber(L, 1);
    lua_pushnumber(L, w_tri(t));
    return 1;
}

// ── draw_waveform ─────────────────────────────────────────────────────────────
//
// Samples the chosen waveform at regular x intervals across the bounding rect
// and draws adjacent samples as line segments.
//
// Sampling density: one sample every 2 pixels of width (capped to at least 4
// samples and at most 2048).  At typical screen widths this gives a smooth curve
// without wasting draw calls.

static int l_draw_waveform(lua_State* L) {
    const char* type_str = luaL_checkstring(L, 1);
    float x      = (float)luaL_checknumber(L, 2);
    float y      = (float)luaL_checknumber(L, 3);
    float w      = (float)luaL_checknumber(L, 4);
    float h      = (float)luaL_checknumber(L, 5);
    float cycles = (float)luaL_optnumber(L, 6, 1.0);
    float phase  = (float)luaL_optnumber(L, 7, 0.0);

    Renderer* r = lua_bindings::get_renderer(L);
    if (!r || w < 1.0f || h < 1.0f) return 0;

    // Identify wave type from string.
    enum { SINE, SAW, SQUARE, TRI, UNKNOWN } wtype = UNKNOWN;
    if      (strcmp(type_str, "sine")   == 0) wtype = SINE;
    else if (strcmp(type_str, "saw")    == 0) wtype = SAW;
    else if (strcmp(type_str, "square") == 0) wtype = SQUARE;
    else if (strcmp(type_str, "tri")    == 0) wtype = TRI;
    else {
        luaL_error(L, "draw_waveform: unknown type '%s' (use 'sine','saw','square','tri')",
                   type_str);
        return 0;
    }

    // Number of samples: one per 2 pixels, clamped.
    int samples = (int)(w * 0.5f) + 1;
    if (samples < 4)    samples = 4;
    if (samples > 2048) samples = 2048;

    // The midline of the waveform sits at y + h/2.
    // The amplitude maps [-1,1] to [-(h/2), +(h/2)].
    // +Y is downward in pixel space, so a positive wave value moves UP on screen.
    float mid_y  = y + h * 0.5f;
    float amp_px = h * 0.5f;

    // Pre-compute the previous sample so we only need one value function call
    // per segment (the current endpoint reuses the previous iteration's endpoint).
    float t0   = phase;
    float frac = (samples > 1) ? (cycles / (float)(samples - 1)) : 0.0f;

    float val0;
    switch (wtype) {
        case SINE:   val0 = w_sine(t0);           break;
        case SAW:    val0 = w_saw(t0);             break;
        case SQUARE: val0 = w_square(t0, 0.5f);   break;
        default:     val0 = w_tri(t0);             break;
    }

    float x0 = x;
    float y0 = mid_y - val0 * amp_px;

    for (int i = 1; i < samples; i++) {
        float t1   = phase + i * frac;
        float val1;
        switch (wtype) {
            case SINE:   val1 = w_sine(t1);          break;
            case SAW:    val1 = w_saw(t1);            break;
            case SQUARE: val1 = w_square(t1, 0.5f);  break;
            default:     val1 = w_tri(t1);            break;
        }

        float x1 = x + (float)i / (float)(samples - 1) * w;
        float y1 = mid_y - val1 * amp_px;

        r->draw_line(x0, y0, x1, y1);

        x0 = x1;
        y0 = y1;
    }

    return 0;
}

// ── Registration ──────────────────────────────────────────────────────────────

void lua_waveform::register_all(lua_State* L) {
    lua_register(L, "wave_sine",       l_wave_sine);
    lua_register(L, "wave_saw",        l_wave_saw);
    lua_register(L, "wave_square",     l_wave_square);
    lua_register(L, "wave_tri",        l_wave_tri);
    lua_register(L, "draw_waveform",   l_draw_waveform);
}
