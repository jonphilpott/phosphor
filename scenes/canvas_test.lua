-- scenes/canvas_test.lua
-- Phase 8 acceptance test: offscreen canvas with local shaders.
--
-- Two canvases are rendered side by side, each with a different local shader.
-- The shaders only affect their own canvas — they do not bleed into each other
-- or into the background.
--
-- canvas 1 (left)  — spinning radial lines  → scanlines applied locally
-- canvas 2 (right) — pulsing circles        → chromatic_ab applied locally
--
-- Both canvases are also drawn small in the top-left corner to verify that
-- scaling (canvas:draw with explicit w/h) works without distortion.

-- Each canvas fills one half of the screen for maximum visibility.
local CW = math.floor(screen_width  / 2)
local CH = math.floor(screen_height)

-- Allocate canvases once at scene load time.
-- Creating canvases inside on_frame every call would leak GPU memory.
local c1 = canvas.new(CW, CH)
local c2 = canvas.new(CW, CH)

local t = 0

function on_frame(dt)
    t = t + dt
    clear(0.03, 0.03, 0.03, 1)

    -- ── Canvas 1 (left half): spinning radial lines + scanlines ──────────
    c1:begin()
        clear(0, 0, 0, 1)
        local cx, cy = CW / 2, CH / 2
        local spokes = 16
        for i = 0, spokes - 1 do
            local angle = t * 0.8 + i * (math.pi * 2 / spokes)
            local hue   = i / spokes
            set_color(0.5 + 0.5 * math.cos(hue * math.pi * 2),
                      0.5 + 0.5 * math.cos(hue * math.pi * 2 + 2.094),
                      0.5 + 0.5 * math.cos(hue * math.pi * 2 + 4.189),
                      1)
            set_stroke_weight(4)
            local ex = cx + math.cos(angle) * (CW * 0.45)
            local ey = cy + math.sin(angle) * (CH * 0.45)
            draw_line(cx, cy, ex, ey)
        end
    c1:finish("scanlines")

    -- ── Canvas 2 (right half): white rings + chromatic aberration ────────
    -- Chromatic aberration splits R/G/B channels apart — needs white or
    -- bright multi-colour content to make the fringing visible.
    c2:begin()
        clear(0, 0, 0, 1)
        local rings = 8
        for i = 1, rings do
            local phase  = t * 1.2 + i * 0.8
            local radius = (i / rings) * (math.min(CW, CH) * 0.45) * (0.85 + 0.15 * math.sin(phase))
            set_color(1, 1, 1, 0.85)   -- white so all three channels are present
            set_stroke_weight(6)
            set_circle_segments(128)
            -- Draw as a bright ring (two circles, inner slightly smaller)
            draw_circle(CW / 2, CH / 2, radius)
            set_color(0, 0, 0, 1)
            draw_circle(CW / 2, CH / 2, radius - 8)
        end
    c2:finish("chromatic_ab")

    -- ── Composite: each canvas fills its half of the screen ───────────────
    c1:draw(0,   0, CW, CH)
    c2:draw(CW,  0, CW, CH)
end
