-- waveform_test.lua
-- Exercises all four wave types in two ways:
--
--   Top half:  draw_waveform() draws each wave as a polyline in its own lane.
--              Phase scrolls with time so you can see the wave moving.
--
--   Bottom half: each wave drives something visual (radius, brightness, position)
--               so you can verify the value functions return sane numbers.
--
-- OSC:
--   /speed  f   -- scroll speed multiplier (default 1)
--   /cycles f   -- cycles displayed across the waveform width (default 3)

local speed  = 1.0
local cycles = 3.0
local t      = 0.0          -- running time in seconds

function on_load()
    -- nothing to preload
end

function on_osc(addr, ...)
    local args = {...}
    if addr == "/speed"  then speed  = args[1] end
    if addr == "/cycles" then cycles = args[1] end
end

function on_frame(dt)
    t = t + dt * speed
    clear(0.03, 0.03, 0.03, 1)

    local sw = screen_width
    local sh = screen_height

    -- ── Section 1: polyline waveforms ────────────────────────────────────────
    -- Divide the top half of the screen into 4 equal horizontal lanes.
    -- Each lane is labelled and drawn with a distinct colour.
    -- Phase is offset by t so the waveform scrolls left.

    local lane_h  = sh * 0.5 / 4       -- height of one lane
    local pad     = 8                   -- vertical padding inside a lane
    local wave_h  = lane_h - pad * 2   -- drawable height within the lane

    local waves = {
        { type = "sine",   label = "sine",   r = 0.2, g = 0.8, b = 0.4 },
        { type = "saw",    label = "saw",    r = 0.9, g = 0.5, b = 0.1 },
        { type = "square", label = "square", r = 0.3, g = 0.6, b = 1.0 },
        { type = "tri",    label = "tri",    r = 0.9, g = 0.2, b = 0.7 },
    }

    set_stroke_weight(1.5)

    for i, w in ipairs(waves) do
        local lane_y = (i - 1) * lane_h   -- top of this lane

        -- Faint lane divider
        set_stroke(0.15, 0.15, 0.15, 1)
        draw_line(0, lane_y, sw, lane_y)

        -- The waveform polyline — phase=t scrolls it
        set_stroke(w.r, w.g, w.b, 1)
        draw_waveform(w.type, 0, lane_y + pad, sw, wave_h, cycles, t)

        -- Label in the top-left of the lane
        -- (no text API yet, so draw a small colour dot as a legend marker)
        set_stroke(w.r, w.g, w.b, 1)
        set_stroke_weight(6)
        draw_point(12, lane_y + lane_h * 0.5)
        set_stroke_weight(1.5)
    end

    -- Divider between top and bottom halves
    set_stroke(0.4, 0.4, 0.4, 1)
    set_stroke_weight(1)
    draw_line(0, sh * 0.5, sw, sh * 0.5)

    -- ── Section 2: value-function demos ──────────────────────────────────────
    -- Four circles arranged horizontally in the bottom half.
    -- Each circle's radius is modulated by one wave value function.
    -- The wave runs at 0.5 Hz (t * 0.5 cycles/sec).

    local base_y   = sh * 0.75          -- vertical centre of demo circles
    local spacing  = sw / 5             -- horizontal spacing
    local base_r   = sh * 0.07          -- base radius at value=0
    local amp_r    = sh * 0.06          -- amplitude: value*amp_r added to base_r
    local freq     = 0.5                -- oscillation frequency in Hz

    local demos = {
        { fn = wave_sine,   r = 0.2, g = 0.8, b = 0.4 },
        { fn = wave_saw,    r = 0.9, g = 0.5, b = 0.1 },
        { fn = wave_square, r = 0.3, g = 0.6, b = 1.0 },
        { fn = wave_tri,    r = 0.9, g = 0.2, b = 0.7 },
    }

    for i, d in ipairs(demos) do
        local cx  = spacing * i
        local val = d.fn(t * freq)           -- returns [-1, 1]
        local rad = base_r + val * amp_r     -- maps to [base_r-amp_r, base_r+amp_r]

        -- Fill brightness also tracks the value so you get a double cue
        local bright = val * 0.4 + 0.6      -- maps [-1,1] → [0.2, 1.0]
        set_color(d.r * bright, d.g * bright, d.b * bright, 1)
        draw_circle(cx, base_y, math.max(rad, 2))

        -- Small dot at rest radius for reference
        set_stroke(d.r * 0.4, d.g * 0.4, d.b * 0.4, 1)
        set_stroke_weight(1)
        -- draw a faint ring at base_r to show the zero-crossing
        -- (no draw_circle_outline, so approximate with a very thin filled circle
        --  drawn on top in background colour)
        set_color(0.03, 0.03, 0.03, 0.5)
        draw_circle(cx, base_y, base_r * 0.25)
    end
end
