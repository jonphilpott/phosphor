// lua_vec.cpp
// 2D vector type exposed to Lua as full userdata.
//
// Why userdata rather than a plain Lua table of {x=, y=}?
//
// A table costs a hash part, a key lookup for every .x and .y, and considerably
// more memory per instance. Userdata here is 16 bytes of payload with the
// components at fixed offsets, and — because we control __index — reading v.x
// is a C function call rather than a hash lookup.
//
// A note on garbage
// ─────────────────
// Every operator returns a NEW vector, which means every `a + b` allocates.
// That is what makes the notation pleasant, and for a few hundred operations a
// frame it is irrelevant. But a particle system doing several thousand vector
// operations per frame generates real garbage, and Lua's incremental collector
// will eventually pay for it in a frame-time spike — exactly the artefact you
// would notice in a 60fps visual.
//
// Hence the trailing-underscore methods: v:add_(u) mutates v and returns it,
// allocating nothing. The convention throughout is:
//
//     v:rotate(a)    -> a new vector, v unchanged      (pure)
//     v:rotate_(a)   -> v itself, rotated              (in place)
//
// Write the readable version first. If a scene ever gets heavy enough to
// matter, the in-place forms are there and the change is mechanical.

#include "lua_vec.h"

#include <cmath>
#include <cstdlib>

extern "C" {
#include "lauxlib.h"
}

static const char* VEC_MT = "phosphor.vec2";

struct Vec2 { float x, y; };

// ── Helpers ───────────────────────────────────────────────────────────────────

static Vec2* check_vec(lua_State* L, int idx) {
    return (Vec2*)luaL_checkudata(L, idx, VEC_MT);
}

static Vec2* test_vec(lua_State* L, int idx) {
    return (Vec2*)luaL_testudata(L, idx, VEC_MT);
}

// Push a new vector onto the stack and return a pointer to it.
static Vec2* push_vec(lua_State* L, float x, float y) {
    Vec2* v = (Vec2*)lua_newuserdata(L, sizeof(Vec2));
    v->x = x;
    v->y = y;
    luaL_setmetatable(L, VEC_MT);
    return v;
}

// Read a vector-shaped argument starting at index `idx`.
//
// Accepts either a vector (one argument) or two numbers, so both of these read
// naturally at the call site:
//
//     p:add_(velocity)
//     p:add_(0, 9.8)
//
// Returns the index just past what it consumed, so callers with trailing
// arguments — lerp's t, smooth's rate and dt — know where those start.
static int read_xy(lua_State* L, int idx, float& x, float& y) {
    if (Vec2* v = test_vec(L, idx)) {
        x = v->x;
        y = v->y;
        return idx + 1;
    }
    x = (float)luaL_checknumber(L, idx);
    y = (float)luaL_checknumber(L, idx + 1);
    return idx + 2;
}

// ── Construction ──────────────────────────────────────────────────────────────

// vec(x, y) / vec.new(x, y) — both default to (0, 0).
static int l_vec_new(lua_State* L) {
    float x = (float)luaL_optnumber(L, 1, 0.0);
    float y = (float)luaL_optnumber(L, 2, 0.0);
    push_vec(L, x, y);
    return 1;
}

// __call on the `vec` table, so `vec(1, 2)` works as well as `vec.new(1, 2)`.
// Argument 1 is the table itself, so the components start at 2.
static int l_vec_call(lua_State* L) {
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    push_vec(L, x, y);
    return 1;
}

// vec.from_angle(a [, len]) — a in radians, measured from +X, counter-clockwise
// in maths convention. Note that +Y is DOWN in screen space, so on screen this
// reads as clockwise; that is a property of the coordinate system, not this
// function.
static int l_vec_from_angle(lua_State* L) {
    float a   = (float)luaL_checknumber(L, 1);
    float len = (float)luaL_optnumber(L, 2, 1.0);
    push_vec(L, cosf(a) * len, sinf(a) * len);
    return 1;
}

// vec.random([len]) — uniformly distributed direction.
//
// Uses Lua's own math.random rather than C rand(), so that a scene calling
// math.randomseed() gets reproducible vectors along with everything else.
static int l_vec_random(lua_State* L) {
    float len = (float)luaL_optnumber(L, 1, 1.0);

    lua_getglobal(L, "math");
    lua_getfield(L, -1, "random");
    lua_call(L, 0, 1);                      // math.random() -> [0,1)
    float u = (float)lua_tonumber(L, -1);
    lua_pop(L, 2);                          // result and the math table

    const float a = u * 6.28318530718f;
    push_vec(L, cosf(a) * len, sinf(a) * len);
    return 1;
}

// ── Operators ─────────────────────────────────────────────────────────────────

static int l_vec_add(lua_State* L) {
    Vec2* a = check_vec(L, 1);
    Vec2* b = check_vec(L, 2);
    push_vec(L, a->x + b->x, a->y + b->y);
    return 1;
}

static int l_vec_sub(lua_State* L) {
    Vec2* a = check_vec(L, 1);
    Vec2* b = check_vec(L, 2);
    push_vec(L, a->x - b->x, a->y - b->y);
    return 1;
}

// __mul handles vec*number, number*vec and vec*vec.
//
// vec*vec is COMPONENT-WISE, not the dot product — use v:dot(u) for that. The
// component-wise form earns its place scaling by a non-uniform factor, e.g.
// p * vec(screen_width, screen_height) to map a 0..1 position onto the screen.
static int l_vec_mul(lua_State* L) {
    Vec2* a = test_vec(L, 1);
    Vec2* b = test_vec(L, 2);

    if (a && b) { push_vec(L, a->x * b->x, a->y * b->y); return 1; }

    // One side is a scalar; which one depends on the operand order.
    Vec2* v = a ? a : b;
    float s = (float)luaL_checknumber(L, a ? 2 : 1);
    push_vec(L, v->x * s, v->y * s);
    return 1;
}

static int l_vec_div(lua_State* L) {
    Vec2* a = check_vec(L, 1);
    if (Vec2* b = test_vec(L, 2)) {
        push_vec(L, a->x / b->x, a->y / b->y);
    } else {
        float s = (float)luaL_checknumber(L, 2);
        push_vec(L, a->x / s, a->y / s);
    }
    return 1;
}

static int l_vec_unm(lua_State* L) {
    Vec2* a = check_vec(L, 1);
    push_vec(L, -a->x, -a->y);
    return 1;
}

// Exact component equality. Floats being floats, two vectors arrived at by
// different routes may differ in the last bit and compare unequal; compare
// a:dist(b) against a small tolerance when that matters.
static int l_vec_eq(lua_State* L) {
    Vec2* a = check_vec(L, 1);
    Vec2* b = check_vec(L, 2);
    lua_pushboolean(L, a->x == b->x && a->y == b->y);
    return 1;
}

static int l_vec_tostring(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    lua_pushfstring(L, "vec(%f, %f)", (double)v->x, (double)v->y);
    return 1;
}

// ── Field access ──────────────────────────────────────────────────────────────
//
// __index serves both v.x / v.y and method lookup. The components are checked
// first because they are by far the most frequent access in a draw loop.

static int l_vec_index(lua_State* L) {
    Vec2* v = check_vec(L, 1);

    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* k = lua_tostring(L, 2);
        if (k[0] && k[1] == '\0') {
            if (k[0] == 'x') { lua_pushnumber(L, v->x); return 1; }
            if (k[0] == 'y') { lua_pushnumber(L, v->y); return 1; }
        }
    }

    // Not a component — look the key up in the methods table, which lives in
    // the metatable under a private field.
    luaL_getmetatable(L, VEC_MT);
    lua_getfield(L, -1, "__methods");
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

static int l_vec_newindex(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    const char* k = luaL_checkstring(L, 2);

    if (k[0] && k[1] == '\0') {
        if (k[0] == 'x') { v->x = (float)luaL_checknumber(L, 3); return 0; }
        if (k[0] == 'y') { v->y = (float)luaL_checknumber(L, 3); return 0; }
    }
    return luaL_error(L, "vec has no field '%s' (only x and y are writable)", k);
}

// ── Scalars out ───────────────────────────────────────────────────────────────

static int l_vec_mag(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    lua_pushnumber(L, sqrtf(v->x * v->x + v->y * v->y));
    return 1;
}

// Squared magnitude. Comparing distances is the common case, and comparing
// squared distances gives the same ordering without the square root.
static int l_vec_mag_sq(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    lua_pushnumber(L, v->x * v->x + v->y * v->y);
    return 1;
}

static int l_vec_dist(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y; read_xy(L, 2, x, y);
    const float dx = v->x - x, dy = v->y - y;
    lua_pushnumber(L, sqrtf(dx * dx + dy * dy));
    return 1;
}

static int l_vec_dist_sq(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y; read_xy(L, 2, x, y);
    const float dx = v->x - x, dy = v->y - y;
    lua_pushnumber(L, dx * dx + dy * dy);
    return 1;
}

static int l_vec_dot(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y; read_xy(L, 2, x, y);
    lua_pushnumber(L, v->x * x + v->y * y);
    return 1;
}

// 2D cross product: the z component of the 3D cross of two vectors in the XY
// plane. Its sign tells you which side of `v` the other vector lies on, which
// is how you steer left or right toward a target.
static int l_vec_cross(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y; read_xy(L, 2, x, y);
    lua_pushnumber(L, v->x * y - v->y * x);
    return 1;
}

static int l_vec_heading(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    lua_pushnumber(L, atan2f(v->y, v->x));
    return 1;
}

// Signed angle from this vector to another, in [-pi, pi].
static int l_vec_angle_to(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y; read_xy(L, 2, x, y);
    lua_pushnumber(L, atan2f(v->x * y - v->y * x, v->x * x + v->y * y));
    return 1;
}

// v:xy() -> x, y
//
// Handy for feeding draw calls, but mind Lua's expansion rule: a call returning
// multiple values is truncated to one unless it is the LAST argument. So
// draw_line(a.x, a.y, b:xy()) works, while draw_circle(p:xy(), r) does NOT —
// that passes (x, r). Write draw_circle(p.x, p.y, r) for the general case.
static int l_vec_xy(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    lua_pushnumber(L, v->x);
    lua_pushnumber(L, v->y);
    return 2;
}

// ── Pure operations (return a new vector) ─────────────────────────────────────

static int l_vec_copy(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    push_vec(L, v->x, v->y);
    return 1;
}

// Scale to unit length. A zero vector has no direction, so it is returned
// unchanged rather than producing NaN.
static void normalize_xy(float& x, float& y) {
    const float m = sqrtf(x * x + y * y);
    if (m > 1e-9f) { x /= m; y /= m; }
}

static int l_vec_normalize(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x = v->x, y = v->y;
    normalize_xy(x, y);
    push_vec(L, x, y);
    return 1;
}

// Cap the magnitude, leaving direction alone. This is the workhorse of any
// steering or physics loop — capping speed, capping a force.
static void limit_xy(float& x, float& y, float max) {
    const float m2 = x * x + y * y;
    if (m2 > max * max && m2 > 1e-18f) {
        const float m = sqrtf(m2);
        x = x / m * max;
        y = y / m * max;
    }
}

static int l_vec_limit(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x = v->x, y = v->y;
    limit_xy(x, y, (float)luaL_checknumber(L, 2));
    push_vec(L, x, y);
    return 1;
}

static void rotate_xy(float& x, float& y, float a) {
    const float c = cosf(a), s = sinf(a);
    const float nx = x * c - y * s;
    y = x * s + y * c;
    x = nx;
}

static int l_vec_rotate(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x = v->x, y = v->y;
    rotate_xy(x, y, (float)luaL_checknumber(L, 2));
    push_vec(L, x, y);
    return 1;
}

static int l_vec_lerp(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y;
    const int next = read_xy(L, 2, x, y);
    const float t  = (float)luaL_checknumber(L, next);
    push_vec(L, v->x + (x - v->x) * t, v->y + (y - v->y) * t);
    return 1;
}

// Frame-rate-independent chase, the vector twin of the scalar smooth().
//
// Chasing a target with lerp(current, target, 0.1) every frame moves twice as
// far at 30fps as at 60fps; multiplying the gap by e^(-rate*dt) does not care
// how the frame rate wanders. This is the single most useful function here for
// animation: almost everything that "follows" something else wants it.
static int l_vec_smooth(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y;
    const int next = read_xy(L, 2, x, y);
    const float rate = (float)luaL_checknumber(L, next);
    const float dt   = (float)luaL_checknumber(L, next + 1);
    const float k    = expf(-rate * dt);
    push_vec(L, x + (v->x - x) * k, y + (v->y - y) * k);
    return 1;
}

// Same, parameterised by the time for the gap to halve — easier to reason
// about in musical time.
static int l_vec_smooth_hl(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y;
    const int next = read_xy(L, 2, x, y);
    const float hl = (float)luaL_checknumber(L, next);
    const float dt = (float)luaL_checknumber(L, next + 1);
    if (hl <= 0.0f) { push_vec(L, x, y); return 1; }   // snap
    const float k = powf(0.5f, dt / hl);
    push_vec(L, x + (v->x - x) * k, y + (v->y - y) * k);
    return 1;
}

// ── In-place operations (mutate and return self) ──────────────────────────────
//
// Each returns the receiver so calls chain: p:add_(vel):limit_(max)

static int self(lua_State* L) { lua_pushvalue(L, 1); return 1; }

static int l_vec_set_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    read_xy(L, 2, v->x, v->y);
    return self(L);
}

static int l_vec_add_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y; read_xy(L, 2, x, y);
    v->x += x; v->y += y;
    return self(L);
}

static int l_vec_sub_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y; read_xy(L, 2, x, y);
    v->x -= x; v->y -= y;
    return self(L);
}

static int l_vec_scale_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    const float s = (float)luaL_checknumber(L, 2);
    v->x *= s; v->y *= s;
    return self(L);
}

static int l_vec_normalize_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    normalize_xy(v->x, v->y);
    return self(L);
}

static int l_vec_limit_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    limit_xy(v->x, v->y, (float)luaL_checknumber(L, 2));
    return self(L);
}

static int l_vec_rotate_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    rotate_xy(v->x, v->y, (float)luaL_checknumber(L, 2));
    return self(L);
}

static int l_vec_lerp_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y;
    const int next = read_xy(L, 2, x, y);
    const float t  = (float)luaL_checknumber(L, next);
    v->x += (x - v->x) * t;
    v->y += (y - v->y) * t;
    return self(L);
}

static int l_vec_smooth_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y;
    const int next = read_xy(L, 2, x, y);
    const float rate = (float)luaL_checknumber(L, next);
    const float dt   = (float)luaL_checknumber(L, next + 1);
    const float k    = expf(-rate * dt);
    v->x = x + (v->x - x) * k;
    v->y = y + (v->y - y) * k;
    return self(L);
}

static int l_vec_smooth_hl_(lua_State* L) {
    Vec2* v = check_vec(L, 1);
    float x, y;
    const int next = read_xy(L, 2, x, y);
    const float hl = (float)luaL_checknumber(L, next);
    const float dt = (float)luaL_checknumber(L, next + 1);
    const float k  = (hl <= 0.0f) ? 0.0f : powf(0.5f, dt / hl);
    v->x = x + (v->x - x) * k;
    v->y = y + (v->y - y) * k;
    return self(L);
}

// ── C++ interface (for lua_persist) ───────────────────────────────────────────

bool lua_vec::get(lua_State* L, int idx, float& x, float& y) {
    Vec2* v = test_vec(L, idx);
    if (!v) return false;
    x = v->x;
    y = v->y;
    return true;
}

void lua_vec::push(lua_State* L, float x, float y) {
    push_vec(L, x, y);
}

// ── Registration ──────────────────────────────────────────────────────────────

void lua_vec::register_all(lua_State* L) {
    static const luaL_Reg methods[] = {
        // scalars out
        { "mag",        l_vec_mag        },
        { "mag_sq",     l_vec_mag_sq     },
        { "dist",       l_vec_dist       },
        { "dist_sq",    l_vec_dist_sq    },
        { "dot",        l_vec_dot        },
        { "cross",      l_vec_cross      },
        { "heading",    l_vec_heading    },
        { "angle_to",   l_vec_angle_to   },
        { "xy",         l_vec_xy         },
        // pure
        { "copy",       l_vec_copy       },
        { "normalize",  l_vec_normalize  },
        { "limit",      l_vec_limit      },
        { "rotate",     l_vec_rotate     },
        { "lerp",       l_vec_lerp       },
        { "smooth",     l_vec_smooth     },
        { "smooth_hl",  l_vec_smooth_hl  },
        // in place
        { "set_",       l_vec_set_       },
        { "add_",       l_vec_add_       },
        { "sub_",       l_vec_sub_       },
        { "scale_",     l_vec_scale_     },
        { "normalize_", l_vec_normalize_ },
        { "limit_",     l_vec_limit_     },
        { "rotate_",    l_vec_rotate_    },
        { "lerp_",      l_vec_lerp_      },
        { "smooth_",    l_vec_smooth_    },
        { "smooth_hl_", l_vec_smooth_hl_ },
        { nullptr, nullptr }
    };

    luaL_newmetatable(L, VEC_MT);

    // Methods live in a table hanging off the metatable; l_vec_index falls back
    // to it after checking for x and y.
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__methods");

    lua_pushcfunction(L, l_vec_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_vec_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, l_vec_add);      lua_setfield(L, -2, "__add");
    lua_pushcfunction(L, l_vec_sub);      lua_setfield(L, -2, "__sub");
    lua_pushcfunction(L, l_vec_mul);      lua_setfield(L, -2, "__mul");
    lua_pushcfunction(L, l_vec_div);      lua_setfield(L, -2, "__div");
    lua_pushcfunction(L, l_vec_unm);      lua_setfield(L, -2, "__unm");
    lua_pushcfunction(L, l_vec_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_vec_tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    // The global `vec` is a table so it can carry constructors, with __call so
    // that vec(1, 2) works as shorthand for vec.new(1, 2).
    lua_newtable(L);
    lua_pushcfunction(L, l_vec_new);        lua_setfield(L, -2, "new");
    lua_pushcfunction(L, l_vec_from_angle); lua_setfield(L, -2, "from_angle");
    lua_pushcfunction(L, l_vec_random);     lua_setfield(L, -2, "random");

    lua_newtable(L);                        // metatable for the vec table
    lua_pushcfunction(L, l_vec_call);       lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);

    lua_setglobal(L, "vec");
}
