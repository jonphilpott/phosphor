#include "lua_text.h"
#include "lua_bindings.h"
#include "font8x8.h"
#include "renderer.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ── text_render::draw ─────────────────────────────────────────────────────────
//
// Rasterises an 8x8 bitmap font into filled rectangles.
//
// The font is stored as 8 bytes per character, one per row, with bit N of a row
// meaning "pixel N from the left is lit". So drawing a glyph is a matter of
// walking the rows and emitting a rectangle wherever bits are set.
//
// Step by step:
//   1. Walk the string one byte at a time.
//   2. '\n' returns the cursor to the starting x and drops one glyph height.
//   3. Bytes outside printable ASCII advance the cursor without drawing.
//   4. For each of the glyph's 8 rows, emit one rectangle per *run* of
//      consecutive set bits (see the comment in the loop for why runs).
//   5. Advance the cursor by one glyph width.
//
// Rectangles go through the renderer's normal path, so the current fill colour
// and the transform stack both apply — text rotates and scales like anything
// else.
void text_render::draw(Renderer& r, float x, float y, const char* str, float scale) {
    const float glyph_w  = 8.0f * scale;
    const float glyph_h  = 8.0f * scale;
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

            // Emit one rectangle per *run* of consecutive set bits rather than
            // one per bit.
            //
            // Each rectangle costs 6 vertices, so drawing pixel-by-pixel spent
            // up to 384 vertices on a single character — and scenes like
            // matrix.lua and datafield.lua put hundreds of characters on screen
            // every frame. Glyph rows are mostly solid strokes: "H" has rows
            // like 11000011 (two runs) and 11111111 (one run), so merging runs
            // typically cuts the vertex count for text by around three times
            // and produces pixel-identical output, since adjacent squares of
            // the same colour and one rectangle spanning them are the same
            // shape.
            int col = 0;
            while (col < 8) {
                // Bit 'col' (counting from the LSB) is the pixel at horizontal
                // position 'col' in this row. Skip clear bits.
                if (!(bits & (1 << col))) { col++; continue; }

                const int run_start = col;
                while (col < 8 && (bits & (1 << col))) col++;

                r.draw_rect(
                    x + run_start * scale,        // pixel X of the run's start
                    y + row * scale,              // pixel Y of this glyph row
                    (col - run_start) * scale,    // one pixel per bit in the run
                    scale
                );
            }
        }

        x += glyph_w;
    }
}

// ── text_render::width ────────────────────────────────────────────────────────
// Pixel width of the longest line: every character is 8 * scale wide, and '\n'
// starts the count again.
float text_render::width(const char* str, float scale) {
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

    return (float)max_chars * 8.0f * scale;
}

// ── Lua bindings ──────────────────────────────────────────────────────────────

// draw_text(x, y, str [, scale])
static int l_draw_text(lua_State* L) {
    float       x     = (float)luaL_checknumber(L, 1);
    float       y     = (float)luaL_checknumber(L, 2);
    const char* str   = luaL_checkstring(L, 3);
    float       scale = (float)luaL_optnumber(L, 4, 1.0);

    Renderer* r = lua_bindings::get_renderer(L);
    if (!r) return 0;

    text_render::draw(*r, x, y, str, scale);
    return 0;
}

// text_width(str [, scale]) → number
// Useful for centring:
//   local w = text_width("HELLO", 2)
//   draw_text(screen_width / 2 - w / 2, y, "HELLO", 2)
static int l_text_width(lua_State* L) {
    const char* str   = luaL_checkstring(L, 1);
    float       scale = (float)luaL_optnumber(L, 2, 1.0);
    lua_pushnumber(L, (double)text_render::width(str, scale));
    return 1;
}

// ── Registration ──────────────────────────────────────────────────────────────

void lua_text::register_all(lua_State* L) {
    lua_register(L, "draw_text",  l_draw_text);
    lua_register(L, "text_width", l_text_width);
}
