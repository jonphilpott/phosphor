#pragma once

extern "C" {
#include "lua.h"
}

namespace lua_easing {
    // Register all easing/utility functions as Lua globals.
    // Call once from lua_bindings::register_all() after the VM is initialised.
    void register_all(lua_State* L);
}
