#pragma once

extern "C" {
#include "lua.h"
}

namespace lua_noise {
    // Register noise() and fbm() as Lua globals.
    // Call once during engine initialisation, after the Lua VM is created.
    void register_all(lua_State* L);
}
