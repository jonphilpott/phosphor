#pragma once

extern "C" {
#include "lua.h"
}

// ── lua_vec ───────────────────────────────────────────────────────────────────
// A 2D vector type for scenes, in the spirit of Processing's PVector.
//
// Lua API:
//
//   vec(x, y)                    -- construct
//   vec.from_angle(a [, len])    -- unit vector at angle a (radians), scaled
//   vec.random([len])            -- random direction
//
//   v.x, v.y                     -- readable and writable
//   a + b   a - b   -a           -- component-wise
//   v * 2   2 * v   a * b        -- scalar or component-wise
//   v / 2   a / b
//   a == b                       -- exact component equality
//
//   -- Pure: return a new vector, leave the receiver alone
//   v:copy()  v:normalize()  v:limit(m)  v:rotate(a)
//   v:lerp(u, t)  v:smooth(target, rate, dt)  v:smooth_hl(target, hl, dt)
//
//   -- In-place: mutate the receiver and return it, for allocation-free loops
//   v:set_(x, y)  v:add_(u)  v:sub_(u)  v:scale_(s)
//   v:normalize_()  v:limit_(m)  v:rotate_(a)
//   v:lerp_(u, t)  v:smooth_(target, rate, dt)  v:smooth_hl_(target, hl, dt)
//
//   -- Scalars out
//   v:mag()  v:mag_sq()  v:dist(u)  v:dist_sq(u)
//   v:dot(u)  v:cross(u)  v:heading()  v:angle_to(u)
//   v:xy()                       -- returns x, y as two values
//
// Anywhere a vector argument is taken, two numbers work as well:
// v:add_(u) and v:add_(1, 0) are both valid.
namespace lua_vec {
    void register_all(lua_State* L);

    // Is the value at `idx` a vector?  If so, write its components to x and y.
    // Used by lua_persist: a vector is two floats and nothing else, so unlike
    // the engine's other userdata types it can be carried across a reload.
    bool get(lua_State* L, int idx, float& x, float& y);

    // Push a new vector onto the stack.
    void push(lua_State* L, float x, float y);
}
