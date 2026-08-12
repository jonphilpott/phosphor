#include "lua_persist.h"
#include "lua_vec.h"

#include <cstdio>

extern "C" {
#include "lauxlib.h"
}

namespace {

// How deeply nested a table may be before we stop copying.
//
// This is a guard against pathological structures, not a style opinion: the
// copy is recursive, and recursion driven by scene data is recursion driven by
// something that can be edited live and got wrong. Sixteen levels is far more
// than any plausible scene state.
constexpr int MAX_DEPTH = 16;

// ── capture_value() ───────────────────────────────────────────────────────────
//
// Copies the value at stack index `idx` into a Value. Returns false if the
// value is of a type that cannot cross the VM boundary, in which case the
// caller skips the key entirely.
//
// `path` holds the table pointers we are currently inside. A table that appears
// in its own path is a cycle — persist.a = persist — and copying it would
// recurse forever, so it is dropped.
bool capture_value(lua_State* L, int idx, lua_persist::Value& out,
                   int depth, std::vector<const void*>& path,
                   lua_persist::Snapshot& stats)
{
    using T = lua_persist::Value::Type;

    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            out.type = T::Nil;
            return true;

        case LUA_TBOOLEAN:
            out.type = T::Boolean;
            out.b    = lua_toboolean(L, idx) != 0;
            return true;

        case LUA_TNUMBER:
            // Lua 5.4 distinguishes integers from floats, and the difference is
            // visible to scenes (string.format("%d"), table indices, //). Keep
            // whichever it actually is rather than flattening to double.
            if (lua_isinteger(L, idx)) {
                out.type = T::Integer;
                out.i    = lua_tointeger(L, idx);
            } else {
                out.type = T::Number;
                out.n    = lua_tonumber(L, idx);
            }
            return true;

        case LUA_TSTRING: {
            // Use the length-aware accessor: Lua strings may contain embedded
            // NULs, and strlen would silently truncate.
            size_t len = 0;
            const char* p = lua_tolstring(L, idx, &len);
            out.type = T::String;
            out.s.assign(p, len);
            return true;
        }

        case LUA_TTABLE: {
            if (depth >= MAX_DEPTH) { stats.dropped_depth++; return false; }

            const void* ptr = lua_topointer(L, idx);
            for (const void* seen : path) {
                if (seen == ptr) { stats.dropped_cycles++; return false; }
            }

            out.type = T::Table;
            path.push_back(ptr);

            // Normalise to a positive index: we push two values per iteration
            // below, which would invalidate a negative (relative) index.
            const int t = lua_absindex(L, idx);

            lua_pushnil(L);                       // first key
            while (lua_next(L, t) != 0) {
                // Stack now: ... key value

                // Only string and number keys cross. Table or userdata keys
                // would have no meaning in a VM where those objects no longer
                // exist, and a boolean key is legal but nobody writes one.
                const int kt = lua_type(L, -2);
                if (kt == LUA_TSTRING || kt == LUA_TNUMBER) {
                    lua_persist::Value k, v;
                    // Key first: capture_value on the key can't recurse (keys
                    // here are scalars) so ordering is only about stack hygiene.
                    if (capture_value(L, -2, k, depth + 1, path, stats) &&
                        capture_value(L, -1, v, depth + 1, path, stats)) {
                        out.entries.emplace_back(std::move(k), std::move(v));
                    }
                } else {
                    stats.dropped_values++;
                }

                lua_pop(L, 1);                    // pop value, keep key for next
            }

            path.pop_back();
            return true;
        }

        case LUA_TUSERDATA: {
            // Vectors are pure data and can cross; every other userdata type in
            // this engine owns a GL object or a malloc'd buffer that dies with
            // the VM, so it cannot.
            float x, y;
            if (lua_vec::get(L, idx, x, y)) {
                out.type = T::Vec2;
                out.vx   = x;
                out.vy   = y;
                return true;
            }
            stats.dropped_values++;
            return false;
        }

        default:
            // Functions, threads, light userdata.
            stats.dropped_values++;
            return false;
    }
}

// ── push_value() ──────────────────────────────────────────────────────────────
// Rebuilds one captured value onto the stack of the new VM.
void push_value(lua_State* L, const lua_persist::Value& v) {
    using T = lua_persist::Value::Type;

    switch (v.type) {
        case T::Nil:     lua_pushnil(L);                            break;
        case T::Boolean: lua_pushboolean(L, v.b ? 1 : 0);           break;
        case T::Integer: lua_pushinteger(L, v.i);                   break;
        case T::Number:  lua_pushnumber(L, v.n);                    break;
        case T::String:  lua_pushlstring(L, v.s.data(), v.s.size()); break;
        case T::Vec2:    lua_vec::push(L, v.vx, v.vy);              break;

        case T::Table:
            // Pre-size the new table so rebuilding a large array doesn't
            // rehash repeatedly.
            lua_createtable(L, 0, (int)v.entries.size());
            for (const auto& kv : v.entries) {
                push_value(L, kv.first);
                push_value(L, kv.second);
                lua_settable(L, -3);
            }
            break;
    }
}

}  // namespace

namespace lua_persist {

void install(lua_State* L) {
    lua_newtable(L);
    lua_setglobal(L, "persist");
}

Snapshot capture(lua_State* L) {
    Snapshot snap;
    snap.table.type = Value::Type::Table;

    if (lua_getglobal(L, "persist") != LUA_TTABLE) {
        // A scene is free to overwrite `persist` with a non-table, or to nil
        // it. Nothing to carry in that case.
        lua_pop(L, 1);
        return snap;
    }

    std::vector<const void*> path;
    capture_value(L, -1, snap.table, 0, path, snap);
    lua_pop(L, 1);
    return snap;
}

void restore(lua_State* L, const Snapshot& snap) {
    push_value(L, snap.table);
    lua_setglobal(L, "persist");
}

std::string describe_losses(const Snapshot& snap) {
    if (!snap.dropped_values && !snap.dropped_cycles && !snap.dropped_depth)
        return {};

    char buf[256];
    int n = snprintf(buf, sizeof(buf), "persist: dropped");
    if (snap.dropped_values)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " %d value(s) that cannot cross a reload "
                      "(function/userdata)", snap.dropped_values);
    if (snap.dropped_cycles)
        n += snprintf(buf + n, sizeof(buf) - n, " %d cyclic table(s)",
                      snap.dropped_cycles);
    if (snap.dropped_depth)
        snprintf(buf + n, sizeof(buf) - n, " %d table(s) nested past depth %d",
                 snap.dropped_depth, MAX_DEPTH);

    return std::string(buf);
}

}  // namespace lua_persist
