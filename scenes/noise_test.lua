-- scenes/noise_test.lua
-- Phase 7 acceptance test: Perlin noise and fbm side by side.
--
-- Left half  — raw Perlin noise:  single octave, relatively sharp features.
-- Right half — fbm (6 octaves):   cloudier, more natural-looking turbulence.
--
-- Both scroll horizontally over time so you can see the field is continuous
-- and seamless (no grid artefacts — that's the point of Perlin vs value noise).

local scale = 0.004    -- spatial zoom: smaller = smoother / zoomed out
local t     = 0        -- time offset scrolls the field to the right
local step  = 24       -- pixel size of each sample square
-- Note: Lua 5.4 has no JIT so per-pixel noise is CPU-bound. step=12 gives
-- ~6× fewer samples than step=6 while still showing the noise field clearly.
-- For full-resolution noise, the right approach is a GLSL shader — the GPU
-- can evaluate noise for every pixel in parallel at negligible cost.

function on_frame(dt)
    t = t + dt * 0.3
    clear(0, 0, 0, 1)

    local half_w = screen_width / 2

    for y = 0, screen_height - 1, step do
        for x = 0, half_w - 1, step do
            -- Raw Perlin: one octave, output is in [-1,1], remap to [0,1].
            local n = noise(x * scale + t, y * scale) * 0.5 + 0.5
            set_color(n, n, n, 1)
            draw_rect(x, y, step, step)

            -- fbm: 6 octaves stacked, each half amplitude / double frequency.
            -- Still approximately in [-1,1] — remap to [0,1] the same way.
            local f = fbm(x * scale + t, y * scale) * 0.5 + 0.5
            -- Tint amber (full red, partial green, no blue) for visual contrast.
            set_color(f, f * 0.6, 0, 1)
            draw_rect(x + half_w, y, step, step)
        end
    end

    -- Dividing line between the two halves
    set_stroke(0.4, 0.4, 0.4, 1)
    set_stroke_weight(1)
    draw_line(half_w, 0, half_w, screen_height)
end
