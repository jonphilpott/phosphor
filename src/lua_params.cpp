#include "lua_params.h"
#include "lua_vec.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
}

namespace {

// Registry key for the table of on() handlers. Using the address of a static
// makes it unique per process with no chance of a name collision.
const char k_handlers_key = '\0';

// A parameter's value, stored outside the Lua VM so it outlives a reload.
struct Param {
    enum class Kind { Number, Vec, List, String, Bool };

    Kind kind = Kind::Number;
    std::vector<double> nums;    // Number:1, Vec:2, List:N
    std::string         str;
    bool                b = false;

    bool has_range = false;
    double min = 0.0, max = 0.0;

    // The heart of the "file wins until OSC touches it" rule. While false, each
    // param() call refreshes the stored value from the default written in the
    // scene file, so editing that default and saving takes effect. Once a
    // message has arrived this latches true and the live value is authoritative.
    bool touched_by_osc = false;

    bool announced = false;      // address printed at startup yet?
};

std::map<std::string, Param> g_params;

double clamp_to(const Param& p, double v) {
    if (!p.has_range) return v;
    return v < p.min ? p.min : (v > p.max ? p.max : v);
}

// ── Reading a default off the Lua stack ───────────────────────────────────────
//
// The default's type is the declaration: it fixes the parameter's kind, and for
// the numeric kinds it fixes how many OSC arguments a message must carry.
bool read_default(lua_State* L, int idx, Param& p) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        p.kind = Param::Kind::Number;
        p.nums.assign(1, lua_tonumber(L, idx));
        return true;
    }
    if (lua_type(L, idx) == LUA_TBOOLEAN) {
        p.kind = Param::Kind::Bool;
        p.b    = lua_toboolean(L, idx) != 0;
        return true;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        p.kind = Param::Kind::String;
        p.str  = lua_tostring(L, idx);
        return true;
    }
    float vx, vy;
    if (lua_vec::get(L, idx, vx, vy)) {
        p.kind = Param::Kind::Vec;
        p.nums = { (double)vx, (double)vy };
        return true;
    }
    if (lua_type(L, idx) == LUA_TTABLE) {
        p.kind = Param::Kind::List;
        p.nums.clear();
        const lua_Integer n = luaL_len(L, idx);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_geti(L, idx, i);
            p.nums.push_back(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        return !p.nums.empty();
    }
    return false;
}

// Push a parameter's current value in the shape its default declared.
void push_value(lua_State* L, const Param& p) {
    switch (p.kind) {
        case Param::Kind::Number: lua_pushnumber(L, p.nums[0]); break;
        case Param::Kind::Bool:   lua_pushboolean(L, p.b);      break;
        case Param::Kind::String: lua_pushstring(L, p.str.c_str()); break;
        case Param::Kind::Vec:
            lua_vec::push(L, (float)p.nums[0], (float)p.nums[1]);
            break;
        case Param::Kind::List:
            lua_createtable(L, (int)p.nums.size(), 0);
            for (size_t i = 0; i < p.nums.size(); ++i) {
                lua_pushnumber(L, p.nums[i]);
                lua_seti(L, -2, (lua_Integer)(i + 1));
            }
            break;
    }
}

// ── param(name, default [, min, max]) ─────────────────────────────────────────
int l_param(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_argcheck(L, lua_gettop(L) >= 2, 2, "param needs a default value");

    auto it = g_params.find(name);
    const bool is_new = (it == g_params.end());

    Param scratch;
    if (!read_default(L, 2, scratch)) {
        return luaL_error(L, "param '%s': default must be a number, vec, "
                             "table of numbers, string or boolean", name);
    }

    if (!lua_isnoneornil(L, 3)) {
        scratch.has_range = true;
        scratch.min = luaL_checknumber(L, 3);
        scratch.max = luaL_optnumber(L, 4, scratch.min);
    }

    if (is_new) {
        // First sight: the default is the value.
        for (double& v : scratch.nums) v = clamp_to(scratch, v);
        it = g_params.emplace(name, std::move(scratch)).first;
    } else {
        Param& p = it->second;

        // Range and kind always follow the file — those are declarations, not
        // values, so an edit to them takes effect immediately.
        p.has_range = scratch.has_range;
        p.min = scratch.min;
        p.max = scratch.max;

        if (!p.touched_by_osc) {
            // Nothing has arrived over OSC for this parameter, so the file is
            // still in charge: adopt whatever default it currently declares.
            p.kind = scratch.kind;
            p.nums = scratch.nums;
            p.str  = scratch.str;
            p.b    = scratch.b;
            for (double& v : p.nums) v = clamp_to(p, v);
        } else if (p.kind != scratch.kind ||
                   (p.kind == Param::Kind::List &&
                    p.nums.size() != scratch.nums.size())) {
            // The scene changed the parameter's shape while a live value was
            // held. The declaration wins — keeping a value of the wrong shape
            // would hand the scene something it cannot use.
            p.kind = scratch.kind;
            p.nums = scratch.nums;
            p.str  = scratch.str;
            p.b    = scratch.b;
            p.touched_by_osc = false;
        }
    }

    if (!it->second.announced) {
        it->second.announced = true;
        printf("[param] /%s\n", name);
    }

    push_value(L, it->second);
    return 1;
}

// ── on(address, fn) ───────────────────────────────────────────────────────────
int l_on(lua_State* L) {
    const char* addr = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (addr[0] != '/') {
        return luaL_error(L, "on: address must start with '/' (got '%s')", addr);
    }

    lua_pushlightuserdata(L, (void*)&k_handlers_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 1);        // address
    lua_pushvalue(L, 2);        // handler
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return 0;
}

}  // namespace

namespace lua_params {

void register_all(lua_State* L) {
    // Fresh handler table for this VM. Handlers are Lua functions, so they
    // cannot outlive the state — scenes re-register them on every load, which
    // happens naturally because on() calls sit at the top level or in on_load.
    lua_pushlightuserdata(L, (void*)&k_handlers_key);
    lua_newtable(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    lua_register(L, "param", l_param);
    lua_register(L, "on",    l_on);

    // Values in g_params deliberately survive: they are the live state of the
    // performance, not of the VM.
}

void dispatch(lua_State* L, const OscMessage& msg) {
    // ── Parameters ───────────────────────────────────────────────────────────
    // The address is /<name>, so strip the leading slash to look it up.
    if (msg.address.size() > 1) {
        auto it = g_params.find(msg.address.c_str() + 1);
        if (it != g_params.end()) {
            Param& p = it->second;

            switch (p.kind) {
                case Param::Kind::Number:
                case Param::Kind::Vec:
                case Param::Kind::List: {
                    // Every component must be present and numeric. A message of
                    // the wrong arity is ignored rather than half-applied,
                    // which would leave the value inconsistent.
                    const size_t want = p.nums.size();
                    if (msg.args.size() < want) break;

                    bool all_numeric = true;
                    for (size_t i = 0; i < want; ++i) {
                        const char t = msg.args[i].type;
                        if (t != 'f' && t != 'i') { all_numeric = false; break; }
                    }
                    if (!all_numeric) break;

                    for (size_t i = 0; i < want; ++i) {
                        const double v = (msg.args[i].type == 'f')
                                       ? (double)msg.args[i].f
                                       : (double)msg.args[i].i;
                        p.nums[i] = clamp_to(p, v);
                    }
                    p.touched_by_osc = true;
                    break;
                }

                case Param::Kind::String:
                    if (!msg.args.empty() && msg.args[0].type == 's') {
                        p.str = msg.args[0].s;
                        p.touched_by_osc = true;
                    }
                    break;

                case Param::Kind::Bool:
                    // Accept an int or a float, since control surfaces disagree
                    // about how to send a switch.
                    if (!msg.args.empty()) {
                        if (msg.args[0].type == 'i') {
                            p.b = msg.args[0].i != 0;
                            p.touched_by_osc = true;
                        } else if (msg.args[0].type == 'f') {
                            p.b = msg.args[0].f != 0.0f;
                            p.touched_by_osc = true;
                        }
                    }
                    break;
            }
        }
    }

    // ── on() handlers ────────────────────────────────────────────────────────
    lua_pushlightuserdata(L, (void*)&k_handlers_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    lua_getfield(L, -1, msg.address.c_str());
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    // Push the message's arguments as separate parameters, so the handler reads
    // as function(level, index) rather than unpacking a table.
    for (const auto& a : msg.args) {
        if      (a.type == 'i') lua_pushinteger(L, a.i);
        else if (a.type == 'f') lua_pushnumber(L,  a.f);
        else if (a.type == 's') lua_pushstring(L,  a.s.c_str());
        else                    lua_pushnil(L);
    }

    if (lua_pcall(L, (int)msg.args.size(), 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        fprintf(stderr, "Lua error [on %s]: %s\n",
                msg.address.c_str(), err ? err : "(no message)");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);   // handler table
}

void reset_to_defaults() {
    // Clearing the latch is enough: the next param() call sees an untouched
    // parameter and adopts the file's default again.
    for (auto& kv : g_params) kv.second.touched_by_osc = false;
    printf("[param] reset to scene defaults\n");
}

int count() { return (int)g_params.size(); }

std::string overlay_text() {
    std::string out;
    char line[160];

    for (const auto& kv : g_params) {
        const Param& p = kv.second;
        // A marker for parameters currently under live control, so you can see
        // at a glance which values the file no longer governs.
        const char* mark = p.touched_by_osc ? "*" : " ";

        switch (p.kind) {
            case Param::Kind::Number:
                snprintf(line, sizeof(line), "%s%-18s %.3f",
                         mark, kv.first.c_str(), p.nums[0]);
                break;
            case Param::Kind::Vec:
                snprintf(line, sizeof(line), "%s%-18s %.2f %.2f",
                         mark, kv.first.c_str(), p.nums[0], p.nums[1]);
                break;
            case Param::Kind::List: {
                int n = snprintf(line, sizeof(line), "%s%-18s",
                                 mark, kv.first.c_str());
                for (double v : p.nums)
                    n += snprintf(line + n, sizeof(line) - n, " %.2f", v);
                break;
            }
            case Param::Kind::String:
                snprintf(line, sizeof(line), "%s%-18s %s",
                         mark, kv.first.c_str(), p.str.c_str());
                break;
            case Param::Kind::Bool:
                snprintf(line, sizeof(line), "%s%-18s %s",
                         mark, kv.first.c_str(), p.b ? "true" : "false");
                break;
        }
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace lua_params
