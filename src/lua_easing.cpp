#include "lua_easing.h"
#include <cmath>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ── lerp(a, b, t) ─────────────────────────────────────────────────────────────
// Linear interpolation: returns the value t of the way from a to b.
//   t=0 → a,  t=1 → b,  t=0.5 → midpoint.
// t is NOT clamped — you can extrapolate by passing t outside [0,1].
static int l_lerp(lua_State* L) {
    double a = luaL_checknumber(L, 1);
    double b = luaL_checknumber(L, 2);
    double t = luaL_checknumber(L, 3);
    lua_pushnumber(L, a + (b - a) * t);
    return 1;
}

// ── smooth(current, target, rate, dt) ─────────────────────────────────────────
// Frame-rate-independent exponential smoothing parameterised by rate.
// Each frame the gap between current and target shrinks by e^(-rate * dt):
//   rate = 1  → ~63% of gap closed per second
//   rate = 5  → ~99% closed per second
//   rate = 10 → nearly instant
//
// Why not lerp(current, target, 0.1) each frame?  That amount moves twice as far
// at 30fps as at 60fps.  Multiplying by e^(-rate*dt) keeps speed frame-rate
// independent regardless of whether dt varies frame to frame.
static int l_smooth(lua_State* L) {
    double current = luaL_checknumber(L, 1);
    double target  = luaL_checknumber(L, 2);
    double rate    = luaL_checknumber(L, 3);
    double dt      = luaL_checknumber(L, 4);
    // As rate or dt grows, exp() approaches 0 and current snaps to target.
    lua_pushnumber(L, target + (current - target) * std::exp(-rate * dt));
    return 1;
}

// ── smooth_hl(current, target, half_life, dt) ─────────────────────────────────
// Same exponential smoothing idea as smooth(), but parameterised by half_life:
// the time in seconds for the gap to halve — more intuitive in musical time.
//   half_life = 0.1 → snappy (gap halves every 100ms)
//   half_life = 0.5 → moderate
//   half_life = 2.0 → very lazy drift
static int l_smooth_hl(lua_State* L) {
    double current   = luaL_checknumber(L, 1);
    double target    = luaL_checknumber(L, 2);
    double half_life = luaL_checknumber(L, 3);
    double dt        = luaL_checknumber(L, 4);
    // Guard against division by zero — if half_life is 0 snap to target.
    if (half_life <= 0.0) {
        lua_pushnumber(L, target);
        return 1;
    }
    // 0.5^(dt/half_life): at dt=half_life the factor is exactly 0.5.
    lua_pushnumber(L, target + (current - target) * std::pow(0.5, dt / half_life));
    return 1;
}

// ── map(x, in_lo, in_hi, out_lo, out_hi) ─────────────────────────────────────
// Remaps x from [in_lo, in_hi] to [out_lo, out_hi].
// Does NOT clamp — use clamp() afterwards if you need hard limits.
// Example: map an OSC value 0..1 to screen X 100..1820:
//   local sx = map(osc_val, 0, 1, 100, 1820)
static int l_map(lua_State* L) {
    double x      = luaL_checknumber(L, 1);
    double in_lo  = luaL_checknumber(L, 2);
    double in_hi  = luaL_checknumber(L, 3);
    double out_lo = luaL_checknumber(L, 4);
    double out_hi = luaL_checknumber(L, 5);
    // Step 1: normalise x to [0,1] within the input range.
    // Step 2: scale that normalised value into the output range.
    lua_pushnumber(L, out_lo + (x - in_lo) * (out_hi - out_lo) / (in_hi - in_lo));
    return 1;
}

// ── clamp(x, lo, hi) ──────────────────────────────────────────────────────────
// Constrains x to [lo, hi].
static int l_clamp(lua_State* L) {
    double x  = luaL_checknumber(L, 1);
    double lo = luaL_checknumber(L, 2);
    double hi = luaL_checknumber(L, 3);
    lua_pushnumber(L, x < lo ? lo : (x > hi ? hi : x));
    return 1;
}

// ── smoothstep(t) ─────────────────────────────────────────────────────────────
// S-curve: maps t in [0,1] to [0,1] with zero derivative at both ends.
// The formula 3t²-2t³ is the classic cubic Hermite interpolant — it looks like
// a gentle ease-in/ease-out rather than the abrupt start/stop of a linear ramp.
static int l_smoothstep(lua_State* L) {
    double t = luaL_checknumber(L, 1);
    // Clamp first so callers don't have to.
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    lua_pushnumber(L, t * t * (3.0 - 2.0 * t));
    return 1;
}

// ── pulse(t, bpm, width) ──────────────────────────────────────────────────────
// Returns a value 0..1 that peaks at 1 on each beat and fades to 0 between.
// Good for flash/strobe effects driven by a time accumulator or u_beat.
//   pulse(t, 120, 0.05) → 120bpm with a 50ms flash window
//
// How it works:
//   1. Convert t to a phase 0..1 within one beat period.
//   2. Measure how far into the beat we are — distance 0 at the downbeat,
//      wrapping so the end of the beat also counts as "near" the next one.
//   3. Smoothstep the distance into a 0..1 pulse width.
static int l_pulse(lua_State* L) {
    double t     = luaL_checknumber(L, 1);
    double bpm   = luaL_checknumber(L, 2);
    double width = luaL_checknumber(L, 3);

    double beat_period = 60.0 / bpm;
    // phase: where we are in the current beat, 0=downbeat, approaching 1.
    double phase = std::fmod(t, beat_period) / beat_period;
    // dist: seconds to the nearest beat boundary (downbeat or next beat).
    double dist  = std::fmin(phase, 1.0 - phase) * beat_period;

    // Invert dist so distance=0 (on the beat) gives smoothstep(1)=1,
    // and distance=width (edge of flash window) gives smoothstep(0)=0.
    double s = 1.0 - dist / width;
    s = s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);  // clamp before smoothstep
    lua_pushnumber(L, s * s * (3.0 - 2.0 * s));
    return 1;
}

// ── register_all() ────────────────────────────────────────────────────────────

void lua_easing::register_all(lua_State* L) {
    lua_register(L, "lerp",      l_lerp);
    lua_register(L, "smooth",    l_smooth);
    lua_register(L, "smooth_hl", l_smooth_hl);
    lua_register(L, "map",       l_map);
    lua_register(L, "clamp",     l_clamp);
    lua_register(L, "smoothstep",l_smoothstep);
    lua_register(L, "pulse",     l_pulse);
}
