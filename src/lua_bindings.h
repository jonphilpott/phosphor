#pragma once

extern "C" {
#include "lua.h"
}

class Renderer;
class ShaderPipeline;

namespace lua_bindings {
    // Register all engine C functions as Lua globals.
    // Call once after the Lua VM is initialised.
    void register_all(lua_State* L);

    // Store the Renderer pointer in the Lua registry so draw bindings can
    // retrieve it.  Call after the Renderer is initialised, before any scene
    // is loaded.
    void set_renderer(lua_State* L, Renderer* r);

    // Retrieve the Renderer pointer stored by set_renderer().
    // Public so other modules (e.g. lua_automata) can issue draw calls.
    Renderer* get_renderer(lua_State* L);

    // Store the ShaderPipeline pointer in the Lua registry so shader_* bindings
    // can retrieve it.  Call after the ShaderPipeline is initialised.
    void set_pipeline(lua_State* L, ShaderPipeline* p);
}
