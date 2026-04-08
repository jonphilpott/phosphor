#include "lua_state.h"
#include "lua_bindings.h"
#include <cstdio>

// ── Constructor / Destructor ──────────────────────────────────────────────────

LuaState::LuaState() = default;

LuaState::~LuaState() {
    if (L) {
        lua_close(L);
        L = nullptr;
    }
}

// ── reset() ───────────────────────────────────────────────────────────────────

void LuaState::reset() {
    // Close the existing VM — this runs __gc on all live userdata (freeing any
    // canvas FBOs, image textures, etc. that the scene allocated) and releases
    // all Lua memory.
    if (L) {
        lua_close(L);
        L = nullptr;
    }
    // Re-create a fresh VM with the same setup as init().
    init();
}

// ── init() ────────────────────────────────────────────────────────────────────

bool LuaState::init() {
    // luaL_newstate() creates a fresh VM with the standard memory allocator.
    // It's the high-level wrapper around lua_newstate() that sets up the
    // panic handler and registry for us.
    L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "Lua: luaL_newstate() failed\n");
        return false;
    }

    // Open all standard libraries (math, string, table, io, os, etc.).
    // This gives scripts access to math.sin, string.format, etc. without
    // any extra work on our part.
    luaL_openlibs(L);

    // Register all engine-provided C functions as Lua globals.
    // The actual functions live in lua_bindings.cpp to keep this file clean.
    lua_bindings::register_all(L);

    printf("Lua %s ready\n", LUA_RELEASE);
    return true;
}

// ── load_file() ───────────────────────────────────────────────────────────────

bool LuaState::load_file(const char* path) {
    // Step 1: luaL_loadfile compiles the file and pushes the compiled chunk
    // as a function onto the stack.  It does NOT execute it yet.
    // Returns LUA_OK (0) on success, non-zero on error.
    int load_err = luaL_loadfile(L, path);
    if (load_err != LUA_OK) {
        report_error(path);
        return false;
    }

    // Step 2: Execute the chunk with lua_pcall.
    // At this point the file's top-level code runs — this is where Lua
    // scripts typically define their functions and initialise their state.
    // 0 args in, LUA_MULTRET results (we discard them), 0 = no error handler.
    int call_err = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (call_err != LUA_OK) {
        report_error(path);
        return false;
    }

    printf("Lua: loaded %s\n", path);
    return true;
}

// ── set_screen_size() ─────────────────────────────────────────────────────────

void LuaState::set_screen_size(int w, int h) {
    // Push the drawable pixel dimensions as Lua globals so scripts can
    // write e.g. local COLS = math.floor(screen_width / 16)
    lua_pushinteger(L, w);
    lua_setglobal(L, "screen_width");
    lua_pushinteger(L, h);
    lua_setglobal(L, "screen_height");
}

// ── call_hook overloads ───────────────────────────────────────────────────────
//
// Pattern used by all overloads:
//   1. lua_getglobal — pushes the value, returns its type
//   2. Check it's a function — if not, pop and return false (not an error)
//   3. Push arguments onto the stack AFTER the function
//   4. lua_pcall(L, nargs, 0, 0) — 0 return values, no message handler
//   5. On error: print message (it's on top of stack), pop, return false

bool LuaState::call_hook(const char* name) {
    if (lua_getglobal(L, name) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        report_error(name);
        return false;
    }
    return true;
}

bool LuaState::call_hook(const char* name, double a) {
    if (lua_getglobal(L, name) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushnumber(L, a);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        report_error(name);
        return false;
    }
    return true;
}

bool LuaState::call_hook(const char* name, double a, double b, double c) {
    if (lua_getglobal(L, name) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushnumber(L, a);
    lua_pushnumber(L, b);
    lua_pushnumber(L, c);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        report_error(name);
        return false;
    }
    return true;
}

bool LuaState::call_hook(const char* name, const char* s, double v) {
    if (lua_getglobal(L, name) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushstring(L, s);
    lua_pushnumber(L, v);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        report_error(name);
        return false;
    }
    return true;
}

// ── report_error() ────────────────────────────────────────────────────────────

void LuaState::report_error(const char* context) {
    // On a lua_pcall error the error message is on top of the stack as a
    // string.  lua_tostring returns a pointer valid while the value is on the
    // stack — copy it via fprintf before popping.
    const char* msg = lua_tostring(L, -1);
    fprintf(stderr, "Lua error [%s]: %s\n", context, msg ? msg : "(no message)");
    lua_pop(L, 1);
}
