#pragma once

extern "C" {
#include "lua.h"
}

// Register the 'wolfram' and 'conway' Lua global tables.
// Call from lua_bindings::register_all() after the Lua VM is initialised.
namespace lua_automata {
    void register_all(lua_State* L);
}
