-- scenes/life.lua
-- Conway's Game of Life, rendered artistically.
--
-- Rules (B3/S23 — the classic):
--   A dead cell with exactly 3 live neighbours is born.
--   A live cell with 2 or 3 live neighbours survives; otherwise it dies.
--
-- The grid is toroidal — left/right and top/bottom edges wrap — so patterns
-- like gliders travel indefinitely without hitting a wall.
--
-- Artistic touches:
--   - Newborn cells (age 1) flash near-white; they settle to the base colour
--     over ~8 generations, so births are visually distinct from survivors.
--   - A subtle feedback trail lets just-dead cells fade rather than vanish,
--     giving the impression of phosphor decay.
--
-- OSC control (port 9000):
--   /randomize  f       — reseed with fill density 0..1 (default 0.3)
--   /speed      f       — generations per second multiplier (default 1.0)
--   /color      r g b   — base colour for aged living cells (default: green)
--
-- SuperCollider:
--
--   ~p = NetAddr("127.0.0.1", 9000);
--   ~p.sendMsg("/randomize", 0.45);      // denser starting population
--   ~p.sendMsg("/speed",     0.5);       // slow-motion
--   ~p.sendMsg("/color",     0.2, 0.6, 1.0);   // blue

local CELL     = 10      -- pixel size of each cell (1 px gap between cells)
local GEN_RATE = 15.0    -- base generations per second

-- ── grid state ───────────────────────────────────────────────────────────────
-- Flat 1-D arrays indexed by  y * cols + x + 1  (1-based, standard Lua).
-- Using flat arrays rather than a table-of-tables is measurably faster in
-- Lua 5.4 because it avoids one extra table dereference per cell access.
local cols, rows          -- grid dimensions, computed from screen size
local state = {}          -- current generation: 1 = alive, 0 = dead
local nxt   = {}          -- next-generation scratch buffer (reused each step)
local age   = {}          -- how many consecutive generations a cell has been alive

local gen_acc  = 0.0      -- fractional-generation time accumulator
local speed_mult = 1.0
local base_r, base_g, base_b = 0.15, 1.0, 0.5   -- aged-cell colour

local last_w, last_h = 0, 0   -- screen size at last grid init (for resize detection)

-- ── OSC ─────────────────────────────────────────────────────────────────────

local randomize_grid   -- forward declaration (defined below, called from on_osc)

function on_osc(addr, ...)
    local args = {...}
    if addr == "/randomize" then
        randomize_grid(args[1] or 0.3)
    elseif addr == "/speed" then
        speed_mult = args[1] or speed_mult
    elseif addr == "/color" then
        base_r = args[1] or base_r
        base_g = args[2] or base_g
        base_b = args[3] or base_b
    end
end

-- ── grid initialisation ──────────────────────────────────────────────────────

-- Fill the grid randomly.  density is the probability that any cell starts alive.
-- age is seeded to a small random value so the entire field doesn't flash white
-- simultaneously on the first frame.
randomize_grid = function(density)
    for i = 1, cols * rows do
        state[i] = (math.random() < density) and 1 or 0
        age[i]   = math.random(0, 5)
        nxt[i]   = 0
    end
end

-- (Re)initialise grid dimensions and state.  Called on load and on resize.
local function init_grid()
    cols = math.floor(screen_width  / CELL)
    rows = math.floor(screen_height / CELL)
    -- Ensure arrays are exactly the right size by filling sequentially.
    for i = 1, cols * rows do
        state[i] = 0
        nxt[i]   = 0
        age[i]   = 0
    end
    randomize_grid(0.3)
    last_w = screen_width
    last_h = screen_height
    gen_acc = 0.0
end

function on_load()
    math.randomseed(os.time())
    init_grid()
end

-- ── simulation step ──────────────────────────────────────────────────────────
-- One generation of B3/S23 applied to the entire grid.
-- We pre-compute the wrapped row offsets outside the inner loop to avoid
-- repeated modulo operations — modulo is relatively expensive in a tight loop.
local function step()
    for y = 0, rows - 1 do
        -- Offset (index base) of each of the three rows involved.
        -- The % operator in Lua always returns a non-negative result when the
        -- divisor is positive, so wrapping at the edges is handled correctly.
        local ym_off = ((y - 1) % rows) * cols
        local yc_off =   y               * cols
        local yp_off = ((y + 1) % rows) * cols

        for x = 0, cols - 1 do
            local xm = (x - 1) % cols
            local xp = (x + 1) % cols

            -- Count live neighbours in the 8-cell Moore neighbourhood.
            local n = state[ym_off + xm + 1] + state[ym_off + x  + 1] + state[ym_off + xp + 1]
                    + state[yc_off + xm + 1]                            + state[yc_off + xp + 1]
                    + state[yp_off + xm + 1] + state[yp_off + x  + 1] + state[yp_off + xp + 1]

            local i = yc_off + x + 1
            local alive = state[i]
            if alive == 1 then
                -- Survival: 2 or 3 neighbours
                nxt[i] = (n == 2 or n == 3) and 1 or 0
            else
                -- Birth: exactly 3 neighbours
                nxt[i] = (n == 3) and 1 or 0
            end
        end
    end

    -- Commit next generation and update age counters in a single pass.
    for i = 1, cols * rows do
        if nxt[i] == 1 then
            age[i] = age[i] + 1
        else
            age[i] = 0
        end
        state[i] = nxt[i]
    end
end

-- ── on_frame ─────────────────────────────────────────────────────────────────

function on_frame(dt)
    -- Rebuild grid if screen dimensions changed (fullscreen toggle, resize).
    if screen_width ~= last_w or screen_height ~= last_h then
        init_grid()
    end

    -- Advance the simulation by the correct number of generations for this frame.
    -- Capped at 4 steps per frame so a momentary hitch doesn't cause a jump.
    gen_acc = math.min(gen_acc + dt * GEN_RATE * speed_mult, 4.0)
    while gen_acc >= 1.0 do
        step()
        gen_acc = gen_acc - 1.0
    end

    -- Clear to black first, then lay a light feedback layer.
    -- The feedback alpha is low (0.18) so dead cells fade within ~2 frames —
    -- just enough to soften the hard disappearance without muddying the field.
    clear(0, 0, 0, 1)
    draw_feedback(0.18, 1.0, 0.0)

    -- Draw living cells.
    -- Colour is interpolated from near-white (newborn) to the base colour (aged).
    -- t = 0 → age 1 (just born, white flash); t = 1 → age ≥ 8 (fully settled).
    for y = 0, rows - 1 do
        local yc_off = y * cols
        local py     = y * CELL
        for x = 0, cols - 1 do
            if state[yc_off + x + 1] == 1 then
                local a = age[yc_off + x + 1]
                local t = math.min(a / 8.0, 1.0)

                -- Linear interpolation from white (1,1,1) to base colour.
                -- At t=0 all channels are 1.0 (white); at t=1 they equal base.
                set_color(1.0 + (base_r - 1.0) * t,
                          1.0 + (base_g - 1.0) * t,
                          1.0 + (base_b - 1.0) * t, 1)

                -- Leave a 1-pixel gap so individual cells are visually distinct.
                draw_rect(x * CELL, py, CELL - 1, CELL - 1)
            end
        end
    end
end
