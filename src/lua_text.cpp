#include "lua_text.h"
#include "lua_bindings.h"
#include "font8x8.h"
#include "renderer.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ── draw_text(x, y, str [, scale]) ───────────────────────────────────────────
// For each character: look up its 8-row glyph, then for each (row, col) where
// the corresponding bit is set, call draw_rect for a scale×scale filled square.
//
// draw_rect uses the current fill colour (set_color) and goes through the
// transform stack — so push/translate/rotate/scale all affect text, same as
// rectangles and circles.
//
// Step-by-step:
//   1. Walk the string one byte at a time.
//   2. '\n' resets the cursor X and advances cursor Y by one glyph height.
//   3. Out-of-range bytes (< 0x20, > 0x7E) advance the cursor without drawing.
//   4. For printable chars, read the glyph from font8x8_basic.
//   5. Inner loops: row 0–7, col 0–7.
//      Test bit 'col' in glyph[row]: if set, draw a filled square.
//   6. Advance cursor by glyph width (8 * scale) after each character.
static int l_draw_text(lua_State* L) {
    Renderer*   r     = lua_bindings::get_renderer(L);
    float       x     = (float)luaL_checknumber(L, 1);
    float       y     = (float)luaL_checknumber(L, 2);
    const char* str   = luaL_checkstring(L, 3);
    float       scale = (float)luaL_optnumber(L, 4, 1.0);

    const float glyph_w = 8.0f * scale;
    const float glyph_h = 8.0f * scale;
    const float origin_x = x;   // saved for '\n' carriage-return

    for (int ci = 0; str[ci]; ci++) {
        unsigned char c = (unsigned char)str[ci];

        if (c == '\n') {
            x  = origin_x;
            y += glyph_h;
            continue;
        }

        // Skip control characters and anything above '~' (DEL, non-ASCII).
        if (c < 0x20 || c > 0x7E) {
            x += glyph_w;
            continue;
        }

        const uint8_t* glyph = font8x8_basic[c];

        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            if (!bits) continue;    // skip blank rows early

            for (int col = 0; col < 8; col++) {
                // Bit 'col' (starting from LSB) represents the pixel
                // at horizontal position 'col' in this row.
                if (bits & (1 << col)) {
                    r->draw_rect(
                        x + col * scale,   // pixel X
                        y + row * scale,   // pixel Y
                        scale,             // pixel width
                        scale              // pixel height
                    );
                }
            }
        }

        x += glyph_w;
    }

    return 0;
}

// ── text_width(str [, scale]) → number ───────────────────────────────────────
// Returns the pixel width of the longest line in str.
// Each character contributes 8 * scale pixels; '\n' resets the line count.
// Useful for centering:
//   local w = text_width("HELLO", 2)
//   draw_text(screen_width / 2 - w / 2, y, "HELLO", 2)
static int l_text_width(lua_State* L) {
    const char* str   = luaL_checkstring(L, 1);
    float       scale = (float)luaL_optnumber(L, 2, 1.0);

    int max_chars = 0;
    int cur_chars = 0;

    for (int i = 0; str[i]; i++) {
        if (str[i] == '\n') {
            if (cur_chars > max_chars) max_chars = cur_chars;
            cur_chars = 0;
        } else {
            cur_chars++;
        }
    }
    if (cur_chars > max_chars) max_chars = cur_chars;

    lua_pushnumber(L, max_chars * 8.0 * (double)scale);
    return 1;
}

// ── Registration ──────────────────────────────────────────────────────────────

void lua_text::register_all(lua_State* L) {
    lua_register(L, "draw_text",  l_draw_text);
    lua_register(L, "text_width", l_text_width);
}
