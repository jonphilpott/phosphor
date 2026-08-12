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

class Renderer;

// ── text_render ───────────────────────────────────────────────────────────────
// The glyph rasteriser behind draw_text, usable from C++ as well as from Lua.
//
// The engine needs to put words on screen itself — the "no scene loaded"
// message, and the error banner when a scene's on_frame throws — and this is
// the only font it has. Keeping the rasteriser separate from the Lua binding
// means both callers share one implementation rather than two that can drift.
namespace text_render {
    // Draw str at pixel (x, y) using the renderer's current fill colour.
    // Glyphs are 8x8; scale multiplies both axes. '\n' starts a new line back
    // at the starting x. Goes through the transform stack like any other
    // primitive.
    void draw(Renderer& r, float x, float y, const char* str, float scale);

    // Pixel width of the longest line in str at the given scale.
    float width(const char* str, float scale);
}

namespace lua_text {
    void register_all(lua_State* L);
}
