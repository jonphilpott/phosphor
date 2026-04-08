#pragma once

// Lua is a C library — we must wrap its headers in extern "C" when including
// from C++ so the compiler knows not to mangle the symbol names for linking.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// LuaState owns a single lua_State and provides:
//   - init()         — create VM, open std libs, register engine bindings
//   - load_file()    — load and execute a .lua scene file
//   - call_hook()    — call a named global Lua function (if it exists)
//   - set_globals()  — push engine state (screen size, etc.) into Lua globals
//
// Design rules:
//   - Every Lua call goes through lua_pcall, NEVER lua_call.
//     A runtime error in a script must not kill the engine during a show.
//   - call_hook silently ignores missing functions (a scene doesn't have to
//     define every hook) but prints errors to stderr on runtime failures.
//   - All Lua calls happen on the main thread.  The OSC recv thread never
//     touches the lua_State.
class LuaState {
public:
    LuaState();
    ~LuaState();

    // Create the VM and register all engine bindings.
    // Returns false on allocation failure (very unlikely).
    bool init();

    // Tear down the current VM and reinitialise a fresh one.
    // All Lua globals, upvalues, and loaded modules are discarded.
    // Engine bindings (renderer, pipeline) must be re-registered after reset().
    // Used by hot reload — GPU state and renderer are unaffected.
    void reset();

    // Load and execute a Lua source file.  The file's globals become
    // available for subsequent call_hook() calls.
    // Returns false and prints the error on syntax or load failure.
    bool load_file(const char* path);

    // Push engine globals that Lua scripts can read as plain variables.
    // Call this after init() and whenever the values change (e.g. resize).
    void set_screen_size(int w, int h);

    // ── call_hook overloads ───────────────────────────────────────────────
    // Each looks up the named global, checks it's a function, pushes args,
    // calls via lua_pcall, and returns true on success.
    // Returns false (without error) if the function isn't defined.

    bool call_hook(const char* name);
    bool call_hook(const char* name, double a);
    bool call_hook(const char* name, double a, double b, double c);
    bool call_hook(const char* name, const char* s, double v);

    // Direct access for bindings that need to push/read values manually.
    // Use with care — the stack must be balanced after every call.
    lua_State* L = nullptr;

private:
    // Print the error string on top of the stack, then pop it.
    void report_error(const char* context);
};
