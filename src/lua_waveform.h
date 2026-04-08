#pragma once

extern "C" {
#include "lua.h"
}

namespace lua_waveform {
    // Register all waveform functions as Lua globals.
    //
    // Value functions (return float in [-1, 1]):
    //   wave_sine(t)
    //   wave_saw(t)
    //   wave_square(t [, duty])
    //   wave_tri(t)
    //
    // Renderer function (draws into the current scene using current stroke colour):
    //   draw_waveform(type, x, y, w, h [, cycles [, phase]])
    //
    // In all functions, t is a phase in cycles (0 = start, 1 = one full period).
    void register_all(lua_State* L);
}
