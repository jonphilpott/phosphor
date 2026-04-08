#pragma once

extern "C" {
#include "lua.h"
}

// ── lua_text ──────────────────────────────────────────────────────────────────
// Registers bitmap text drawing functions into the Lua state.
//
// Lua API:
//
//   draw_text(x, y, str [, scale])
//     Draw str at pixel position (x, y) using the current fill colour
//     (set with set_color).  Each glyph is 8×8 pixels; scale multiplies
//     both axes (default 1).  Respects the transform stack.
//     '\n' advances to the next line.
//
//   text_width(str [, scale])  → number
//     Returns the pixel width of str at the given scale (default 1).
//     For multi-line strings, returns the width of the longest line.
//     Each character is 8 * scale pixels wide.
//
// Example:
//   set_color(1, 1, 0, 1)                -- yellow
//   draw_text(10, 10, "HELLO", 3)        -- 3× scale, 24px tall glyphs
//   local w = text_width("HELLO", 3)     -- 40 * 3 = 120
//   draw_text(screen_width/2 - w/2, 50, "HELLO", 3)  -- horizontally centred

namespace lua_text {
    void register_all(lua_State* L);
}
