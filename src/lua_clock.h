#pragma once

extern "C" {
#include "lua.h"
}

// ── lua_clock ─────────────────────────────────────────────────────────────────
//
// Musical time, inferred from /beat messages.
//
// The problem: /beat arrives as discrete events. A scene can flash *on* a beat,
// but between messages nothing musical is happening — so you cannot drive
// anything *by* the beat. A rotation that completes exactly once per bar, an
// envelope that decays over an eighth note, a value that eases across two
// beats: none of those are expressible from a value that only changes 2-3 times
// a second.
//
// So this module watches when /beat messages arrive, infers the tempo from the
// intervals between them, and exposes a phase that advances continuously in
// between. Musical time becomes something you can read every frame:
//
//     rotate(bar_phase() * math.pi * 2)     -- exactly one turn per bar
//     local flash = env("kick", 0.12)       -- decays over 120ms after a trigger
//
// The clock free-runs at the inferred tempo, so a dropped packet does not stall
// the visuals — the phase keeps advancing and re-syncs when the next beat lands.
//
// State lives here in C++ rather than in Lua, so tempo and phase survive a
// scene reload. Saving a file mid-set must not restart the clock.
namespace lua_clock {

void register_all(lua_State* L);

// Called by the engine when a /beat message arrives, and once per frame with
// the wall-clock time in seconds. Wall clock deliberately, not the engine's
// scaled clock: pausing or slowing the visuals must not distort musical time.
void on_beat(double wall_seconds);
void update(double wall_seconds);

// Continuous position within the current beat, [0,1). Exposed to C++ so the
// shader pipeline can hand it to fragment shaders as u_beat_phase.
float beat_phase();

}  // namespace lua_clock
