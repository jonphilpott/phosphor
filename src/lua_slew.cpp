#include "lua_slew.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

extern "C" {
#include "lauxlib.h"
}

namespace {

// Registry key for the per-VM table of handles. Using the address of a static
// makes it unique per process with no chance of a name collision — the same
// trick lua_params uses for its on() handler table.
const char k_handles_key = '\0';

// One slewed value, stored outside the Lua VM so it outlives a reload.
struct Slew {
    double value     = 0.0;   // where it is now
    double target    = 0.0;   // where it is heading
    double slew_time = 0.0;   // seconds to cover 99% of the remaining gap

    // The "file wins until set()" latch, the same idea as Param::touched_by_osc.
    // While false, each slew() call refreshes value and target from the default
    // written in the scene file, so editing that default and saving takes
    // effect. The first set() latches this true and the live value is then
    // authoritative — a save mid-glide will not snap it back.
    bool touched = false;
};

// Keyed by name and deliberately never cleared: this is the live state of the
// performance, not of the VM. It survives lua_close() and hot reload.
std::map<std::string, Slew> g_slews;

// The handle closures carry the name as an upvalue rather than a pointer into
// g_slews, because a std::map rehash or a reload could invalidate a pointer
// while the handle lives on. A string lookup per call is cheap at scene scale.
Slew* find_by_upvalue(lua_State* L) {
    const char* name = lua_tostring(L, lua_upvalueindex(1));
    auto it = g_slews.find(name);
    return (it == g_slews.end()) ? nullptr : &it->second;
}

// foo.set(v) — retarget. The glide starts from wherever the value is now.
int l_slew_set(lua_State* L) {
    // Tolerate foo:set(v) as well as foo.set(v). The colon form passes the
    // handle table as the first argument, which would otherwise fail in
    // luaL_checknumber with a message about the handle rather than the value.
    // vec uses colon methods, so this typo is a matter of muscle memory.
    const int idx = lua_istable(L, 1) ? 2 : 1;
    const double v = luaL_checknumber(L, idx);

    if (Slew* s = find_by_upvalue(L)) {
        s->target  = v;
        s->touched = true;

        // A zero slew time means "no glide", so apply it here rather than
        // waiting for the next update(). Otherwise get() right after set()
        // would still read the old value, and the very first frame — where the
        // engine reports dt = 0 — would not move it at all.
        if (s->slew_time <= 0.0) s->value = v;
    }
    return 0;
}

// foo.get() — the current, smoothed value. Also bound as __call, so foo()
// reads it too. Arguments are ignored, which is what makes the __call binding
// work unchanged: the metamethod is handed the handle as its first argument.
int l_slew_get(lua_State* L) {
    const Slew* s = find_by_upvalue(L);
    lua_pushnumber(L, s ? s->value : 0.0);
    return 1;
}

// print(foo) and tostring(foo) show the value rather than "table: 0x...".
int l_slew_tostring(lua_State* L) {
    const Slew* s = find_by_upvalue(L);
    lua_pushfstring(L, "slew(%s: %f)",
                    lua_tostring(L, lua_upvalueindex(1)), s ? s->value : 0.0);
    return 1;
}

// Push the handle for `name`, building it on first use and caching it in the
// registry thereafter.
//
// The cache matters: slew() is designed to be called every frame, and without
// it each call would allocate a fresh table, three closures and a metatable
// sixty times a second. With it, the second and every later call is one table
// lookup.
void push_handle(lua_State* L, const char* name) {
    // ── 1. The per-VM handle cache ───────────────────────────────────────────
    lua_pushlightuserdata(L, (void*)&k_handles_key);
    lua_rawget(L, LUA_REGISTRYINDEX);          // stack: handles

    lua_getfield(L, -1, name);                 // stack: handles, cached-or-nil
    if (lua_istable(L, -1)) {
        lua_remove(L, -2);                     // stack: handle
        return;
    }
    lua_pop(L, 1);                             // stack: handles

    // ── 2. Build the handle: a table of two closures ─────────────────────────
    // Each closure captures the name as upvalue 1. That is the entire binding
    // between the Lua-side handle and the C++-side state.
    lua_newtable(L);                           // stack: handles, handle

    lua_pushstring(L, name);
    lua_pushcclosure(L, l_slew_set, 1);
    lua_setfield(L, -2, "set");

    lua_pushstring(L, name);
    lua_pushcclosure(L, l_slew_get, 1);
    lua_setfield(L, -2, "get");

    // ── 3. Metatable: foo() as a terser alias for foo.get() ──────────────────
    // A bare `foo` cannot be the value — Lua has no __tonumber metamethod, so
    // draw_circle(x, y, foo) would still reach luaL_checknumber and fail.
    // Arithmetic metamethods would make `foo * 2` work while that case stayed
    // broken, which is a worse trap than no magic at all. So: __call only.
    lua_newtable(L);                           // stack: handles, handle, meta

    lua_pushstring(L, name);
    lua_pushcclosure(L, l_slew_get, 1);
    lua_setfield(L, -2, "__call");

    lua_pushstring(L, name);
    lua_pushcclosure(L, l_slew_tostring, 1);
    lua_setfield(L, -2, "__tostring");

    lua_setmetatable(L, -2);                   // stack: handles, handle

    // ── 4. Cache it, then leave only the handle on the stack ─────────────────
    lua_pushvalue(L, -1);                      // stack: handles, handle, handle
    lua_setfield(L, -3, name);                 // handles[name] = handle
    lua_remove(L, -2);                         // stack: handle
}

// slew(name, default [, slew_time]) -> handle
//
// Declares and reads in one call, so it is safe to call every frame.
int l_slew(lua_State* L) {
    const char*  name = luaL_checkstring(L, 1);
    const double def  = luaL_checknumber(L, 2);
    const double st   = luaL_optnumber(L, 3, 0.25);

    luaL_argcheck(L, st >= 0.0, 3, "slew time cannot be negative");

    auto it = g_slews.find(name);
    if (it == g_slews.end()) {
        // First sight: the default is both where it is and where it is heading.
        Slew s;
        s.value     = def;
        s.target    = def;
        s.slew_time = st;
        g_slews.emplace(name, s);
    } else {
        Slew& s = it->second;

        // The slew time is a declaration, not a value, so it always follows the
        // file — retune the glide, save, and the new timing applies at once
        // even to a slew that is already live.
        s.slew_time = st;

        // The value only follows the file until the scene takes control of it.
        if (!s.touched) {
            s.value  = def;
            s.target = def;
        }
    }

    push_handle(L, name);
    return 1;
}

}  // namespace

namespace lua_slew {

void register_all(lua_State* L) {
    // Fresh handle cache for this VM. The handles are Lua tables holding C
    // closures, so they cannot outlive the state; the values they point at, in
    // g_slews, deliberately do.
    lua_pushlightuserdata(L, (void*)&k_handles_key);
    lua_newtable(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    lua_register(L, "slew", l_slew);
}

// Advance every slew toward its target.
//
// The maths is one line, but it earns some explanation.
//
// The naive way to chase a target is `value += (target - value) * 0.1` every
// frame, which is wrong in a way that only shows up when the frame rate moves:
// at 30fps it takes exactly twice as big a step as at 60fps, so the same scene
// glides at different speeds on different machines.
//
// The fix is to ask how much of the gap should REMAIN after dt seconds, which
// is an exponential decay:
//
//     remaining(dt) = 0.01 ^ (dt / slew_time)
//
// 0.01 is the fraction left after one full slew time — hence "slew_time is the
// time to cover 99% of the distance". This form is frame-rate independent
// because exponents add: covering dt=a then dt=b multiplies the two factors
// together, giving exactly the same result as one step of dt=a+b. Two 8ms
// frames land in precisely the same place as one 16ms frame.
void update(double dt) {
    // Paused (dt == 0) needs no special case — the factor would come out as 1
    // and nothing would move — but there is no point walking the map for it.
    if (dt <= 0.0) return;

    for (auto& entry : g_slews) {
        Slew& s = entry.second;
        if (s.value == s.target) continue;

        // A zero slew time means "no glide", matching what smooth_hl() does
        // with a zero half-life. Guarding it also keeps dt/0 out of pow().
        if (s.slew_time <= 0.0) {
            s.value = s.target;
            continue;
        }

        s.value = s.target + (s.value - s.target) * std::pow(0.01, dt / s.slew_time);

        // An exponential approaches its target but never mathematically
        // arrives, so snap once the gap stops mattering. The threshold is
        // relative to the target so it behaves the same for an 0..1 knob as for
        // a value measured in hundreds of pixels.
        const double eps = 1e-6 * std::max(1.0, std::fabs(s.target));
        if (std::fabs(s.value - s.target) < eps) s.value = s.target;
    }
}

void reset_to_defaults() {
    // Only clear the latches. The values themselves come back on the next
    // slew() call, which is the one place that knows what the file says.
    for (auto& entry : g_slews) entry.second.touched = false;
}

}  // namespace lua_slew
