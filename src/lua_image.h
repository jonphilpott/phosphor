#pragma once

extern "C" {
#include "lua.h"
}

namespace lua_image {
    // Register image.load() and sprite_sheet.new() as Lua globals.
    // Call once during engine initialisation, after the Lua VM is created.
    void register_all(lua_State* L);
}
