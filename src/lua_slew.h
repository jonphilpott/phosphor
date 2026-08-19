#pragma once

extern "C" {
#include "lua.h"
}

// ── lua_slew ──────────────────────────────────────────────────────────────────
//
// Named variables that glide to a new value instead of jumping to it.
//
// The problem: a scene that wants a value to ease toward a target has to do
// three separate jobs by hand. Find somewhere to park the current value between
// frames, remember to advance it every frame, and thread dt through to wherever
// that happens:
//
//     function on_load()
//         persist.hue = persist.hue or 0.55      -- 1. park it somewhere
//         persist.hue_target = persist.hue
//     end
//
//     function on_frame(dt)
//         persist.hue = smooth_hl(persist.hue,   -- 2. advance it, every frame
//                                 persist.hue_target, 0.15, dt)   -- 3. with dt
//         set_color_hsv(persist.hue, 0.8, 1.0)
//     end
//
// That is three lines of bookkeeping and two variables for what is conceptually
// one value. Worse, it is fragile: forget the smooth_hl call in one code path
// and the value silently stops moving.
//
// ── slew(name, default, slew_time) ───────────────────────────────────────────
//
//     local hue = slew("hue", 0.55, 0.15)
//     hue.set(0.9)                                 -- retarget from anywhere
//     set_color_hsv(hue.get(), 0.8, 1.0)           -- read the smoothed value
//
// The engine advances every slew once per frame, so the scene never sees dt at
// all. Like param(), this declares and reads in one call and is safe to call
// every frame — a repeat call with the same name hands back the same handle
// onto the same stored state, so it can live right where the value is used.
//
// Note that `hue` on its own is the handle, not the value: Lua has no
// __tonumber metamethod, so a bare handle passed to draw_circle() would fail on
// the C side. Read it with .get() or by calling it, hue().
//
// ── The curve ────────────────────────────────────────────────────────────────
//
// Exponential, the same shape as smooth_hl() and env(). It moves fastest when
// furthest away and eases in as it arrives, and retargeting mid-glide simply
// bends the curve rather than producing a kink. `slew_time` is the time to
// cover 99% of the remaining distance; a slew_time of 0 snaps instantly.
//
// Slews run on the engine's scaled clock, the same dt that on_frame receives.
// Pausing freezes a glide where it stands and the time-scale keys stretch it,
// so a slew behaves like every other visual gesture on screen.
//
// ── Reload ───────────────────────────────────────────────────────────────────
//
// Values live outside the Lua VM, so they survive a reload, and the rule is the
// same one param() uses: the file wins until set() is called. A slew tracks the
// default written in the source — so editing that default and saving does what
// you expect — until the scene calls set() on it, after which the live value
// sticks and a save mid-glide will not snap it back. The slew time is a
// declaration rather than a value, so it always follows the file: retune the
// glide, save, and the new timing applies immediately.
namespace lua_slew {

// Register the `slew` global. Called for every new VM.
void register_all(lua_State* L);

// Advance every slew by dt seconds. Called once per frame by the engine.
// Takes no lua_State: the state lives outside the VM, like the musical clock.
void update(double dt);

// Forget every set() and fall back to the defaults written in the scene file.
// Bound to the same key as the equivalent for parameters.
void reset_to_defaults();

}  // namespace lua_slew
