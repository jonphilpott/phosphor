-- scenes/matrix.lua
-- Digital rain using geometric glyphs instead of characters.
--
-- Each screen column runs an independent stream of falling shapes.
-- Three glyph types cycle and flicker: dot, square, octagon.
-- The head of each stream flashes white; the trail fades exponentially.
--
-- OSC control (port 9000):
--   /speed  f       — global speed multiplier (default 1.0)
--   /color  r g b   — trail colour as floats 0..1 (default: phosphor green)
--
-- SuperCollider:
--
--   ~p = NetAddr("127.0.0.1", 9000);
--   ~p.sendMsg("/speed", 1.5);
--   ~p.sendMsg("/color", 0.0, 1.0, 0.4);   // green
--   ~p.sendMsg("/color", 0.2, 0.6, 1.0);   // cold blue

local CELL = 16          -- pixel grid size: each glyph occupies a CELL×CELL slot

local cols       = {}    -- array of column state tables, one per screen column
local last_width = 0    -- screen_width at last column build; 0 forces init on first frame

-- OSC-controllable globals
local speed_mult        = 1.0
local col_r, col_g, col_b = 0.0, 1.0, 0.4   -- phosphor green default

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

-- ── column factory ───────────────────────────────────────────────────────────
-- Each column holds:
--   x        — pixel x of the left edge of this column slot
--   head_y   — pixel y of the leading (bottom) glyph
--   speed    — pixels per second this stream falls
--   trail    — number of glyph cells in the trail behind the head
--   glyphs   — table mapping row-index → glyph type (1=dot, 2=square, 3=octagon)
--   flip_t   — seconds until the next random glyph is shuffled
--
-- Glyph table is large enough to cover the screen plus the maximum trail,
-- indexed by row (math.floor(py / CELL) mod num_rows + 1).
local function make_col(x)
    local num_rows = math.floor(screen_height / CELL) + 40
    local c = {
        x      = x,
        head_y = math.random(-screen_height, -CELL),  -- start above screen
        speed  = math.random(80, 220),
        trail  = math.random(8, 28),
        glyphs = {},
        flip_t = math.random() * 0.15,   -- stagger initial shuffles
    }
    for i = 1, num_rows do
        c.glyphs[i] = math.random(1, 3)
    end
    return c
end

function on_load()
    math.randomseed(os.time())
    -- last_width starts at 0, so the resize check in on_frame will build
    -- the column array on the very first frame using the correct screen_width.
end

-- ── resize handling ──────────────────────────────────────────────────────────
-- Called at the top of on_frame whenever screen_width has changed.
-- Adds columns to fill new space (fullscreen expand) or removes excess
-- columns from the right when the window shrinks.
local function sync_cols()
    local need = math.floor(screen_width / CELL)
    local have = #cols

    if need > have then
        -- Wider — append new columns for the extra space on the right.
        for i = have + 1, need do
            cols[i] = make_col((i - 1) * CELL)
        end
    elseif need < have then
        -- Narrower — drop columns that are now off-screen.
        for i = need + 1, have do
            cols[i] = nil
        end
    end

    last_width = screen_width
end

-- ── glyph drawing ────────────────────────────────────────────────────────────
-- All three glyphs are sized relative to CELL so they scale if CELL changes.
-- bright is 0..1 and multiplies the base trail colour.
local function draw_glyph(gtype, cx, cy, bright)
    local r = col_r * bright
    local g = col_g * bright
    local b = col_b * bright

    if gtype == 1 then
        -- Dot: a single thick point, cleanest at small sizes
        set_stroke(r, g, b, 1)
        set_stroke_weight(4)
        draw_point(cx, cy)

    elseif gtype == 2 then
        -- Square: filled rect, slightly inset from the cell boundary
        local half = CELL * 0.28
        set_color(r, g, b, 1)
        draw_rect(cx - half, cy - half, half * 2, half * 2)

    else
        -- Octagon: circle with 8 segments — looks angular and "digital"
        -- Low segment count is intentional here; it reads as a shape, not a blob.
        set_color(r, g, b, 1)
        set_circle_segments(8)
        draw_circle(cx, cy, CELL * 0.32)
    end
end

-- ── on_frame ─────────────────────────────────────────────────────────────────

function on_frame(dt)
    -- Rebuild column list if screen_width has changed (fullscreen toggle, resize).
    if screen_width ~= last_width then sync_cols() end

    clear(0, 0, 0, 1)

    -- num_rows must match make_col() — used for the glyph table wrap
    local num_rows = math.floor(screen_height / CELL) + 40

    for _, c in ipairs(cols) do

        -- ── advance ──────────────────────────────────────────────────────────
        c.head_y = c.head_y + c.speed * speed_mult * dt

        -- Once the entire trail (including the tail cell) has cleared the
        -- bottom of the screen, reset the column to a random position above it.
        if c.head_y - c.trail * CELL > screen_height then
            c.head_y = math.random(-math.floor(screen_height * 0.6), -CELL)
            c.speed  = math.random(80, 220)
            c.trail  = math.random(8, 28)
        end

        -- ── glyph shuffle ────────────────────────────────────────────────────
        -- Replace one random cell in this column's glyph table every 50–150 ms.
        -- This is what creates the "static" flicker on the trail — individual
        -- cells silently change type between frames.
        c.flip_t = c.flip_t - dt
        if c.flip_t <= 0 then
            c.flip_t = 0.05 + math.random() * 0.10
            local idx = math.random(1, num_rows)
            c.glyphs[idx] = math.random(1, 3)
        end

        -- ── draw trail ───────────────────────────────────────────────────────
        -- pos=0 is the head (bottom of the active region), pos=trail is the top.
        -- We draw head→tail so the head is painted last and sits on top.
        local cx = c.x + CELL * 0.5   -- horizontal centre of this column

        for pos = c.trail, 0, -1 do
            local py = c.head_y - pos * CELL   -- pixel y of this cell

            -- Skip cells that are entirely off screen
            if py >= -CELL and py <= screen_height + CELL then

                if pos == 0 then
                    -- Head: always pure white regardless of trail colour,
                    -- drawn as a bright dot so it reads as the "leading edge"
                    set_stroke(1, 1, 1, 1)
                    set_stroke_weight(5)
                    draw_point(cx, py)
                else
                    -- Trail: exponential brightness falloff from head to tail.
                    -- The exponent 2.2 gives a curve that stays bright near the
                    -- head and drops off quickly — matches phosphor decay.
                    local frac   = pos / c.trail      -- 0..1 (head..tail)
                    local bright = (1.0 - frac) ^ 2.2

                    -- Row index into this column's glyph table.
                    -- Modulo wraps large row numbers back into the table size.
                    -- Lua's % always returns non-negative when divisor > 0.
                    local row = math.floor(py / CELL) % num_rows + 1

                    draw_glyph(c.glyphs[row], cx, py, bright)
                end
            end
        end
    end
end
