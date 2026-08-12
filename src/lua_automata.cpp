#include "lua_automata.h"
#include "lua_bindings.h"   // for lua_bindings::get_renderer
#include "renderer.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ─────────────────────────────────────────────────────────────────────────────
// Wolfram 1D Elementary Cellular Automaton
// ─────────────────────────────────────────────────────────────────────────────
//
// A "rule" is an 8-bit number (0-255) that defines how every possible 3-cell
// neighbourhood maps to the next cell state.  For each of the 8 possible
// 3-bit patterns (left, centre, right), the rule bit at that position gives
// the output.  Rule 30 produces chaotic output; Rule 90 gives a Sierpinski
// triangle; Rule 110 is Turing-complete.
//
// We store a circular buffer of `num_rows` rows.  Each step advances the head
// pointer by one slot (overwriting the oldest row), so the buffer holds the
// last num_rows generations at all times.

static const char* WOLFRAM_MT = "phosphor.wolfram";

struct WolframCA {
    int      rule;
    int      width;
    int      num_rows;   // capacity of the history ring buffer
    int      head;       // index of the most recent (newest) row
    int      filled;     // how many rows are currently valid (0..num_rows)
    uint8_t* cells;      // malloc'd [num_rows * width]
};

static WolframCA* check_wolfram(lua_State* L) {
    return (WolframCA*)luaL_checkudata(L, 1, WOLFRAM_MT);
}

// Advance one generation: read from `head`, write to the next slot.
static void wolfram_step_impl(WolframCA* ca) {
    int w = ca->width;
    const uint8_t rule = (uint8_t)ca->rule;

    // Source: current newest row.
    const uint8_t* src = ca->cells + ca->head * w;

    // Destination: the slot that will become the new head.
    int next_head = (ca->head + 1) % ca->num_rows;
    uint8_t* dst  = ca->cells + next_head * w;

    // Apply the rule to every cell.  The neighbourhood wraps at the edges so
    // the automaton behaves as if it were on a ring (no boundary artefacts).
    for (int c = 0; c < w; c++) {
        int l = (c > 0)     ? c - 1 : w - 1;   // left neighbour, wrapping
        int r = (c < w - 1) ? c + 1 : 0;        // right neighbour, wrapping

        // Build the 3-bit index into the rule lookup: left=bit2, centre=bit1, right=bit0.
        int pattern = (src[l] << 2) | (src[c] << 1) | src[r];
        dst[c] = (rule >> pattern) & 1;
    }

    ca->head = next_head;
    if (ca->filled < ca->num_rows) ca->filled++;
}

// ── Lua methods ───────────────────────────────────────────────────────────────

// wolfram.new(rule, width [, rows]) → userdata
// rule:  0-255
// width: cells per row
// rows:  history depth (default 256)
static int l_wolfram_new(lua_State* L) {
    int rule  = (int)luaL_checkinteger(L, 1);
    int width = (int)luaL_checkinteger(L, 2);
    int rows  = (int)luaL_optinteger(L, 3, 256);

    // The upper bounds matter as much as the lower ones: `rows * width` below is
    // an int multiply, so unbounded values overflow to a negative number, which
    // calloc then reinterprets as a colossal size_t.  Capping each dimension at
    // 65535 keeps the product comfortably inside int range (and 65535 cells is
    // already far more than any display can show).
    luaL_argcheck(L, rule  >= 0 && rule  <= 255,   1, "rule must be 0-255");
    luaL_argcheck(L, width >= 1 && width <= 65535, 2, "width must be 1-65535");
    luaL_argcheck(L, rows  >= 1 && rows  <= 65535, 3, "rows must be 1-65535");

    // Allocate the userdata struct (stored on the Lua heap).
    //
    // Order matters here.  luaL_error longjmps out of this function, so the
    // object must already be in a state __gc can handle safely *before* anything
    // that might fail: zero it first (so cells is null, and free(null) is a
    // no-op), then attach the metatable, then do the risky allocation.
    WolframCA* ca = (WolframCA*)lua_newuserdata(L, sizeof(WolframCA));
    memset(ca, 0, sizeof(WolframCA));
    ca->rule     = rule;
    ca->width    = width;
    ca->num_rows = rows;

    luaL_setmetatable(L, WOLFRAM_MT);

    // Allocate the cell buffer separately (can be large — keep off Lua heap).
    // The size_t casts keep the multiply out of int range; the argchecks above
    // already bound it, and the casts make that guarantee explicit here.
    ca->cells = (uint8_t*)calloc((size_t)rows * (size_t)width, 1);
    if (!ca->cells) luaL_error(L, "wolfram: out of memory (%d bytes)", rows * width);
    return 1;
}

// ca:seed([pos])
// Sets one cell to 1 and clears the rest.  pos is 1-indexed; defaults to centre.
static int l_wolfram_seed(lua_State* L) {
    WolframCA* ca = check_wolfram(L);
    int pos = (int)luaL_optinteger(L, 2, ca->width / 2 + 1) - 1;  // 1→0 index
    pos = (pos < 0) ? 0 : (pos >= ca->width ? ca->width - 1 : pos);

    // Clear entire buffer and reset pointers.
    memset(ca->cells, 0, ca->num_rows * ca->width);
    ca->head   = 0;
    ca->filled = 1;

    ca->cells[pos] = 1;   // row 0 is the initial generation
    return 0;
}

// ca:randomize()
// Fills the initial row with random 0/1 values.
static int l_wolfram_randomize(lua_State* L) {
    WolframCA* ca = check_wolfram(L);
    memset(ca->cells, 0, (size_t)ca->num_rows * (size_t)ca->width);
    ca->head   = 0;
    ca->filled = 1;
    // Take a middle bit rather than bit 0: several C libraries implement rand()
    // as a linear congruential generator whose low bit alternates in a short
    // cycle, which would give a visibly striped starting row instead of noise.
    for (int c = 0; c < ca->width; c++)
        ca->cells[c] = (rand() >> 16) & 1;
    return 0;
}

// ca:step()
// Compute one new generation and add it to the history ring.
static int l_wolfram_step(lua_State* L) {
    wolfram_step_impl(check_wolfram(L));
    return 0;
}

// ca:draw(x, y, cell_w, cell_h)
// Draw all stored history rows from oldest (top) to newest (bottom).
// Uses the current fill colour set by set_color().
static int l_wolfram_draw(lua_State* L) {
    WolframCA* ca = check_wolfram(L);
    float ox  = (float)luaL_checknumber(L, 2);
    float oy  = (float)luaL_checknumber(L, 3);
    float cw  = (float)luaL_optnumber(L, 4, 4.0);
    float ch  = (float)luaL_optnumber(L, 5, 4.0);

    Renderer* r = lua_bindings::get_renderer(L);
    if (!r || ca->filled == 0) return 0;

    int draw_count = ca->filled;  // how many rows to actually render

    for (int i = 0; i < draw_count; i++) {
        // Map visual row i (0 = oldest, draw_count-1 = newest) to the ring index.
        // When the buffer isn't full yet, ring layout matches visual order directly.
        // When full, oldest = (head + 1) % num_rows.
        int buf_row;
        if (ca->filled < ca->num_rows) {
            buf_row = i;
        } else {
            buf_row = (ca->head + 1 + i) % ca->num_rows;
        }

        const uint8_t* row = ca->cells + buf_row * ca->width;
        float row_y = oy + i * ch;

        for (int c = 0; c < ca->width; c++) {
            if (row[c]) {
                r->draw_rect(ox + c * cw, row_y, cw, ch);
            }
        }
    }
    return 0;
}

// ca:get(i) → 0 or 1    (1-indexed, reads from the current/newest row)
static int l_wolfram_get(lua_State* L) {
    WolframCA* ca = check_wolfram(L);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    luaL_argcheck(L, i >= 0 && i < ca->width, 2, "column out of range");
    lua_pushinteger(L, ca->cells[ca->head * ca->width + i]);
    return 1;
}

// ca:set(i, v)    (1-indexed, writes into the current/newest row)
static int l_wolfram_set(lua_State* L) {
    WolframCA* ca = check_wolfram(L);
    int i = (int)luaL_checkinteger(L, 2) - 1;
    int v = (int)luaL_checkinteger(L, 3);
    luaL_argcheck(L, i >= 0 && i < ca->width, 2, "column out of range");
    ca->cells[ca->head * ca->width + i] = v ? 1 : 0;
    return 0;
}

// __gc: free the cell buffer when the userdata is collected.
static int l_wolfram_gc(lua_State* L) {
    WolframCA* ca = (WolframCA*)luaL_checkudata(L, 1, WOLFRAM_MT);
    free(ca->cells);
    ca->cells = nullptr;
    return 0;
}

// __tostring for debugging.
static int l_wolfram_tostring(lua_State* L) {
    WolframCA* ca = check_wolfram(L);
    lua_pushfstring(L, "wolfram(rule=%d, width=%d, rows=%d)",
                    ca->rule, ca->width, ca->num_rows);
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Conway's Game of Life  (2D, toroidal boundaries)
// ─────────────────────────────────────────────────────────────────────────────
//
// The standard B3/S23 rules: a dead cell with exactly 3 live neighbours becomes
// alive; a live cell with 2 or 3 live neighbours survives; all others die.
//
// We keep two identically-sized buffers (cur and nxt) so we can compute the
// whole next generation before committing any changes.  After each step, the
// pointers are swapped — no extra allocation needed per frame.

static const char* CONWAY_MT = "phosphor.conway";

struct ConwayCA {
    int      width;
    int      height;
    uint8_t* cur;    // current generation  [width * height]
    uint8_t* nxt;    // scratch buffer for next generation
};

static ConwayCA* check_conway(lua_State* L) {
    return (ConwayCA*)luaL_checkudata(L, 1, CONWAY_MT);
}

static void conway_step_impl(ConwayCA* ca) {
    const int w = ca->width, h = ca->height;

    // The obvious way to write this — a nested dx/dy loop doing
    // (x + dx + w) % w and (y + dy + h) % h for each of the 8 neighbours —
    // costs eight integer divisions per cell. On a 320×180 grid that is about
    // 460,000 divisions per generation, and integer division is one of the
    // slowest instructions a CPU has. It is also entirely avoidable:
    //
    //   - The three row offsets are the same for every cell in a row, so they
    //     are computed once per row instead of eight times per cell.
    //   - Along a row, the wrap only ever happens at the two ends, so the left
    //     and right indices are tracked incrementally rather than recomputed.
    //
    // Same toroidal topology, same B3/S23 rules, no modulo in the inner loop.
    for (int y = 0; y < h; y++) {
        // Row offsets for the row above, this row, and the row below, wrapping
        // top-to-bottom. Computed once per row.
        const int y_up   = (y == 0)     ? (h - 1) : (y - 1);
        const int y_down = (y == h - 1) ? 0       : (y + 1);
        const uint8_t* row_up   = ca->cur + (size_t)y_up   * w;
        const uint8_t* row_mid  = ca->cur + (size_t)y      * w;
        const uint8_t* row_down = ca->cur + (size_t)y_down * w;
        uint8_t*       row_out  = ca->nxt + (size_t)y      * w;

        for (int x = 0; x < w; x++) {
            // Column indices either side, wrapping left-to-right.
            const int x_left  = (x == 0)     ? (w - 1) : (x - 1);
            const int x_right = (x == w - 1) ? 0       : (x + 1);

            const int n = row_up  [x_left] + row_up  [x] + row_up  [x_right]
                        + row_mid [x_left] +                row_mid [x_right]
                        + row_down[x_left] + row_down[x] + row_down[x_right];

            // B3/S23: born on 3, survives on 2 or 3.
            row_out[x] = row_mid[x] ? (n == 2 || n == 3 ? 1 : 0)
                                    : (n == 3 ? 1 : 0);
        }
    }

    // Swap buffers — O(1), no copying.
    uint8_t* tmp = ca->cur;
    ca->cur      = ca->nxt;
    ca->nxt      = tmp;
}

// ── Lua methods ───────────────────────────────────────────────────────────────

// conway.new(width, height) → userdata
static int l_conway_new(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    // Upper bounds guard the `w * h` multiply below against int overflow, which
    // would otherwise reach calloc as a wrapped-around size.
    luaL_argcheck(L, w >= 1 && w <= 65535, 1, "width must be 1-65535");
    luaL_argcheck(L, h >= 1 && h <= 65535, 2, "height must be 1-65535");

    // Zero and attach the metatable before allocating, so a failure partway
    // through leaves an object whose __gc can run harmlessly (see the same
    // pattern in l_wolfram_new above).
    ConwayCA* ca = (ConwayCA*)lua_newuserdata(L, sizeof(ConwayCA));
    memset(ca, 0, sizeof(ConwayCA));
    ca->width  = w;
    ca->height = h;

    luaL_setmetatable(L, CONWAY_MT);

    const size_t cells = (size_t)w * (size_t)h;
    ca->cur = (uint8_t*)calloc(cells, 1);
    ca->nxt = (uint8_t*)calloc(cells, 1);
    if (!ca->cur || !ca->nxt) {
        luaL_error(L, "conway: out of memory (%dx%d)", w, h);
    }

    return 1;
}

// ca:randomize([density])
// density = fraction of live cells, 0.0-1.0 (default 0.35)
static int l_conway_randomize(lua_State* L) {
    ConwayCA* ca    = check_conway(L);
    float density   = (float)luaL_optnumber(L, 2, 0.35);
    int threshold   = (int)(density * RAND_MAX);

    for (int i = 0; i < ca->width * ca->height; i++)
        ca->cur[i] = (rand() < threshold) ? 1 : 0;
    return 0;
}

// ca:clear()
static int l_conway_clear(lua_State* L) {
    ConwayCA* ca = check_conway(L);
    memset(ca->cur, 0, ca->width * ca->height);
    return 0;
}

// ca:step()
static int l_conway_step(lua_State* L) {
    conway_step_impl(check_conway(L));
    return 0;
}

// ca:draw(x, y, cell_w, cell_h)
// Draws live cells as filled squares using the current fill colour.
static int l_conway_draw(lua_State* L) {
    ConwayCA* ca = check_conway(L);
    float ox = (float)luaL_checknumber(L, 2);
    float oy = (float)luaL_checknumber(L, 3);
    float cw = (float)luaL_optnumber(L, 4, 4.0);
    float ch = (float)luaL_optnumber(L, 5, 4.0);

    Renderer* r = lua_bindings::get_renderer(L);
    if (!r) return 0;

    int w = ca->width, h = ca->height;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (ca->cur[y * w + x]) {
                r->draw_rect(ox + x * cw, oy + y * ch, cw, ch);
            }
        }
    }
    return 0;
}

// ca:get(x, y) → 0 or 1   (1-indexed)
static int l_conway_get(lua_State* L) {
    ConwayCA* ca = check_conway(L);
    int x = (int)luaL_checkinteger(L, 2) - 1;
    int y = (int)luaL_checkinteger(L, 3) - 1;
    luaL_argcheck(L, x >= 0 && x < ca->width,  2, "x out of range");
    luaL_argcheck(L, y >= 0 && y < ca->height, 3, "y out of range");
    lua_pushinteger(L, ca->cur[y * ca->width + x]);
    return 1;
}

// ca:set(x, y, v)   (1-indexed)
static int l_conway_set(lua_State* L) {
    ConwayCA* ca = check_conway(L);
    int x = (int)luaL_checkinteger(L, 2) - 1;
    int y = (int)luaL_checkinteger(L, 3) - 1;
    int v = (int)luaL_checkinteger(L, 4);
    luaL_argcheck(L, x >= 0 && x < ca->width,  2, "x out of range");
    luaL_argcheck(L, y >= 0 && y < ca->height, 3, "y out of range");
    ca->cur[y * ca->width + x] = v ? 1 : 0;
    return 0;
}

// __gc
static int l_conway_gc(lua_State* L) {
    ConwayCA* ca = (ConwayCA*)luaL_checkudata(L, 1, CONWAY_MT);
    free(ca->cur); free(ca->nxt);
    ca->cur = ca->nxt = nullptr;
    return 0;
}

static int l_conway_tostring(lua_State* L) {
    ConwayCA* ca = check_conway(L);
    lua_pushfstring(L, "conway(%d x %d)", ca->width, ca->height);
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

// Helper: create a named metatable, populate it with methods, set __index = mt.
static void make_metatable(lua_State* L, const char* name,
                            const luaL_Reg* methods,
                            lua_CFunction gc_fn,
                            lua_CFunction tostring_fn)
{
    // luaL_newmetatable pushes (or retrieves) a table stored in the Lua registry
    // under `name`.  We use this as both the metatable AND the method table by
    // setting __index = mt.  This means `obj:method()` looks up `method` in the
    // metatable itself — one table instead of two.
    luaL_newmetatable(L, name);

    // __index = the metatable itself, enabling method dispatch via obj:method()
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    // Register all methods into the metatable.
    luaL_setfuncs(L, methods, 0);

    // __gc: called by the Lua garbage collector to free C-side memory.
    lua_pushcfunction(L, gc_fn);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, tostring_fn);
    lua_setfield(L, -2, "__tostring");

    lua_pop(L, 1);  // pop the metatable — it lives in the registry, not the stack
}

void lua_automata::register_all(lua_State* L) {
    // ── Wolfram ───────────────────────────────────────────────────────────────
    static const luaL_Reg wolfram_methods[] = {
        { "seed",       l_wolfram_seed      },
        { "randomize",  l_wolfram_randomize },
        { "step",       l_wolfram_step      },
        { "draw",       l_wolfram_draw      },
        { "get",        l_wolfram_get       },
        { "set",        l_wolfram_set       },
        { nullptr, nullptr }
    };
    make_metatable(L, WOLFRAM_MT, wolfram_methods, l_wolfram_gc, l_wolfram_tostring);

    // Expose wolfram.new() as a global table with a `new` constructor.
    // This gives Lua the Processing-style `wolfram.new(rule, width)` syntax.
    lua_newtable(L);
    lua_pushcfunction(L, l_wolfram_new);
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "wolfram");

    // ── Conway ────────────────────────────────────────────────────────────────
    static const luaL_Reg conway_methods[] = {
        { "randomize",  l_conway_randomize  },
        { "clear",      l_conway_clear      },
        { "step",       l_conway_step       },
        { "draw",       l_conway_draw       },
        { "get",        l_conway_get        },
        { "set",        l_conway_set        },
        { nullptr, nullptr }
    };
    make_metatable(L, CONWAY_MT, conway_methods, l_conway_gc, l_conway_tostring);

    lua_newtable(L);
    lua_pushcfunction(L, l_conway_new);
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "conway");
}
