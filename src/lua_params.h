#pragma once

#include "osc.h"

#include <string>

extern "C" {
#include "lua.h"
}

// ── lua_params ────────────────────────────────────────────────────────────────
//
// Two ways to receive OSC without writing a dispatch ladder.
//
// Before this, every scene that wanted OSC wrote the same shape by hand:
//
//     function on_osc(addr, ...)
//         local args = {...}
//         if     addr == "/left_vu"  then ...
//         elseif addr == "/scroll"   then ...
//         elseif addr == "/feedback" then ...
//         end
//     end
//
// which grows a branch per address, unpacks arguments positionally as args[1]
// and args[2], and repeats across every scene.
//
// ── on(address, fn) — events ─────────────────────────────────────────────────
//
//     on("/left_vu", function(level, index) ... end)
//
// Arguments arrive named. Adding an address never touches existing code. This
// is the right tool when a message *does* something — picks a target, fires an
// envelope, sets several bits of state at once.
//
// ── param(name, default [, min, max]) — values ───────────────────────────────
//
//     local alpha = param("feedback_alpha", 0.0, 0, 1)
//     local scroll = param("scroll", vec(0, 0))
//
// Declares and reads in one call, so it can live directly where the value is
// used and is safe to call every frame. The parameter listens on /<name>, and
// the DEFAULT'S TYPE decides how many OSC arguments it consumes:
//
//     number   -> 1 float           /speed 1.5
//     vec      -> 2 floats          /scroll 10 -4
//     table    -> N floats          /color 1.0 0.2 0.6
//     string   -> 1 string          /preset "warm"
//     boolean  -> 1 int or bool     /invert 1
//
// Values live outside the Lua VM, so they survive a reload. The rule is that
// the file wins until OSC takes over: a parameter tracks the default written in
// the source — so editing that default and saving does what you expect — until
// a message arrives for it, after which the live value sticks. That way live
// control is never stomped by a reload mid-set.
//
// Both mechanisms observe messages rather than consuming them: anything handled
// here still reaches on_osc, so adding a param or a handler can never silently
// break an existing scene.
namespace lua_params {

// Register the `on` and `param` globals. Called for every new VM.
void register_all(lua_State* L);

// Offer one incoming message to the parameter store and the handler registry.
// Called by the engine before on_osc. Never consumes the message.
void dispatch(lua_State* L, const OscMessage& msg);

// Forget every OSC-supplied value, so parameters fall back to the defaults
// written in the scene file. Bound to a key for "put it back how it was".
void reset_to_defaults();

// Number of declared parameters, and a formatted "name value" line for each —
// used by the on-screen overlay and the startup address listing.
int  count();
std::string overlay_text();

}  // namespace lua_params
