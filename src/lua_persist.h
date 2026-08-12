#pragma once

#include <string>
#include <vector>
#include <memory>

extern "C" {
#include "lua.h"
}

// ── lua_persist ───────────────────────────────────────────────────────────────
//
// Carries the contents of the global `persist` table across a hot reload.
//
// The problem this solves: reloading a scene destroys the entire Lua VM
// (lua_close, then a fresh luaL_newstate). That is what makes reload reliable —
// no stale globals, no half-updated closures — but it also means the scene
// starts from nothing every time the file is saved. A clock accumulating
// `t = t + dt`, a particle field, a Game of Life grid that has been running for
// ten minutes: all gone on a one-character edit.
//
// So before the VM is closed we walk `persist` and copy it into plain C++
// values, and after the new VM is built we rebuild the table inside it. The
// scene sees its state still there:
//
//     persist.t = (persist.t or 0) + dt
//
// What can cross, and what cannot
// ───────────────────────────────
// Numbers, strings, booleans and tables of those (nested) cross fine.
//
// Vectors cross too: a vec is two
// floats and owns nothing, so carrying it is just carrying its components — and
// storing positions in persist is the obvious thing to want.
//
// Functions, threads and the other userdata types cannot. Canvases, images,
// sprite sheets and automata own resources tied to the VM being destroyed: GL
// objects freed by __gc, malloc'd buffers freed by __gc. There is no honest way
// to carry those across, so they are dropped and reported rather than silently
// vanishing. A scene that wants an automaton's grid to survive can copy it into
// a plain table with the get/set methods those types already provide.
namespace lua_persist {

// One captured Lua value. Deliberately a plain tree of C++ data with no
// reference to the lua_State it came from — that state is about to cease to
// exist, which is the entire point.
struct Value {
    enum class Type { Nil, Boolean, Integer, Number, String, Table, Vec2 };

    Type        type = Type::Nil;
    bool        b    = false;
    lua_Integer i    = 0;
    lua_Number  n    = 0.0;
    std::string s;
    float       vx = 0.0f, vy = 0.0f;   // Type::Vec2

    // Key/value pairs for Type::Table. A vector rather than a map because Lua
    // table order is unspecified anyway and we only ever walk it once.
    std::vector<std::pair<Value, Value>> entries;
};

// A captured `persist` table, plus a note of anything that had to be dropped.
struct Snapshot {
    Value table;                  // always Type::Table (possibly empty)
    int   dropped_values  = 0;    // functions/userdata/threads that can't cross
    int   dropped_cycles  = 0;    // table cycles
    int   dropped_depth   = 0;    // nesting past the depth limit
    bool  empty() const { return table.entries.empty(); }
};

// Create an empty global `persist` table. Called on every VM init so the table
// always exists and a scene never has to nil-check it before assigning.
void install(lua_State* L);

// Copy the global `persist` table out of L. Safe to call on a VM with no such
// global (yields an empty snapshot).
Snapshot capture(lua_State* L);

// Rebuild `persist` inside L from a snapshot. Must be called after the new VM
// is created and before the scene file runs, so top-level scene code sees it.
void restore(lua_State* L, const Snapshot& snap);

// Human-readable summary of anything dropped, or empty if nothing was.
std::string describe_losses(const Snapshot& snap);

}  // namespace lua_persist
