#pragma once

extern "C" {
#include "lua.h"
}

class Renderer;
class ShaderPipeline;

#include <vector>

namespace lua_bindings {
    // Shared argument readers for the uniform setters, so the global and the
    // canvas-local versions parse identically.
    int read_uniform_args(lua_State* L, int idx, float* out);   // -> component count
    int read_data_table(lua_State* L, int idx, std::vector<float>& out);

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
