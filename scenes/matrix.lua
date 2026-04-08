-- scenes/matrix.lua
-- Digital rain: falling streams of flickering ASCII characters.
--
-- Each screen column runs an independent stream.  The head character
-- is always white; the trail fades exponentially and characters shuffle
-- randomly to create the "static" flicker effect.
--
-- OSC control (port 9000):
--   /speed  f       — global speed multiplier (default 1.0)
--   /color  r g b   — trail colour as floats 0..1 (default: phosphor green)
--
-- SuperCollider:
--   ~p = NetAddr("127.0.0.1", 9000);
--   ~p.sendMsg("/speed", 1.5);
--   ~p.sendMsg("/color", 0.0, 1.0, 0.4);   -- green
--   ~p.sendMsg("/color", 0.2, 0.6, 1.0);   -- cold blue

local CELL  = 32   -- pixel size of each grid cell — matches scale-2 glyph exactly
local SCALE = 4    -- text scale: each glyph renders as 16×16 pixels

-- Characters that appear in the rain.  Deliberately mixed: numbers, caps,
-- and symbols give that dense, technical look even without katakana.
local CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%&*+=<>?|/\\"

local function rand_char()
    local i = math.random(1, #CHARS)
    return string.sub(CHARS, i, i)   -- same index for both bounds = exactly one character
end

local cols        = {}
local last_width  = 0
local last_height = 0

local speed_mult        = 1.0
local col_r, col_g, col_b = 0.0, 1.0, 0.4

function on_load()
    math.randomseed(os.time())
    shader_set("glitch", "chromatic_ab")
end

function on_osc(addr, ...)
    local args = {...}
    if addr == "/speed" then
        speed_mult = args[1] or speed_mult
    elseif addr == "/color" then
        col_r = args[1] or col_r
        col_g = args[2] or col_g
        col_b = args[3] or col_b
    end
end

-- ── column factory ────────────────────────────────────────────────────────────
-- glyphs[] maps a world-row-index to a character string.
-- Indexed by floor(py / CELL) % num_rows + 1 so each world row has its own
-- character, and the same row shows the same character regardless of where
-- the stream head is.  Shuffling randomly replaces one entry each flip.
local function make_col(x)
    local num_rows = math.floor(screen_height / CELL) + 40
    local c = {
        x      = x,
        head_y = math.random(-screen_height, -CELL),
        speed  = math.random(80, 220),
        trail  = math.random(8, 28),
        glyphs = {},
        flip_t = math.random() * 0.15,
    }
    for i = 1, num_rows do
        c.glyphs[i] = rand_char()
    end
    return c
end

-- ── resize handling ───────────────────────────────────────────────────────────
local function sync_cols()
    -- If the screen height changed, all existing glyph tables are undersized
    -- (they were built with the old num_rows).  Accessing beyond the table end
    -- returns nil, which crashes draw_text.  The safest fix is a full rebuild.
    if screen_height ~= last_height then
        cols = {}
    end

    local need = math.floor(screen_width / CELL)
    local have = #cols
    if need > have then
        for i = have + 1, need do
            cols[i] = make_col((i - 1) * CELL)
        end
    elseif need < have then
        for i = need + 1, have do cols[i] = nil end
    end
    last_width  = screen_width
    last_height = screen_height
end

-- ── on_frame ──────────────────────────────────────────────────────────────────

function on_frame(dt)
    if screen_width ~= last_width or screen_height ~= last_height then
        sync_cols()
    end

    clear(0, 0, 0, 1)

    local num_rows = math.floor(screen_height / CELL) + 40

    for _, c in ipairs(cols) do

        -- Advance head downward
        c.head_y = c.head_y + c.speed * speed_mult * dt

        -- Reset stream once the whole trail has cleared the bottom
        if c.head_y - c.trail * CELL > screen_height then
            c.head_y = math.random(-math.floor(screen_height * 0.6), -CELL)
            c.speed  = math.random(80, 220)
            c.trail  = math.random(8, 28)
        end

        -- Shuffle one random cell in the glyph table every 50–150 ms.
        -- This silently changes individual characters between frames —
        -- the source of the flicker on the trail.
        c.flip_t = c.flip_t - dt
        if c.flip_t <= 0 then
            c.flip_t = 0.05 + math.random() * 0.10
            c.glyphs[math.random(1, num_rows)] = rand_char()
        end

        -- Draw from tail to head so the head character always sits on top
        for pos = c.trail, 0, -1 do
            local py = c.head_y - pos * CELL

            if py >= -CELL and py <= screen_height + CELL then
                -- World-row index: consistent per y position regardless of
                -- where the stream head is, so characters don't "scroll up"
                -- as the stream moves — they flicker in place.
                local row  = math.floor(py / CELL) % num_rows + 1
                local char = c.glyphs[row]

                if pos == 0 then
                    -- Head: pure white, slightly brighter than the trail peak
                    set_color(1, 1, 1, 1)
                else
                    -- Trail: exponential falloff — bright near head, fast drop.
                    -- Exponent 2.2 mimics phosphor persistence decay.
                    local frac   = pos / c.trail
                    local bright = (1.0 - frac) ^ 2.2
                    set_color(col_r * bright, col_g * bright, col_b * bright, 1)
                end

                -- draw_text takes top-left corner.  At SCALE=2 each glyph is
                -- exactly CELL×CELL pixels, so c.x / py fill the cell cleanly.
                draw_text(c.x, py, char, SCALE)
            end
        end
    end
end
