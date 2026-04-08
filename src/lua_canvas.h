#pragma once

extern "C" {
#include "lua.h"
}

namespace lua_canvas {
    // Register canvas.new() as a Lua global.
    // Call once during engine initialisation, after the Lua VM is created.
    void register_all(lua_State* L);
}
