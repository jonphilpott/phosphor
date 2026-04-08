-- scenes/sprite_test.lua
-- Phase 7 acceptance test: sprite sheet frame cycling.
--
-- Expects:  assets/walk.png   — a sprite sheet with 64×64 px frames,
--           laid out left-to-right then top-to-bottom.
--           Drop any compatible sheet in assets/ and adjust frame_w/frame_h.
--
-- The scene cycles through every frame at `fps` frames per second and draws
-- the current frame centred and scaled up 4×.

local sheet
local frame   = 1
local elapsed = 0
local fps     = 12      -- animation playback speed in frames per second
local frame_w = 64
local frame_h = 64
local scale   = 4       -- display scale multiplier

function on_load()
    sheet = sprite_sheet.new("assets/walk.png", frame_w, frame_h)
end

function on_frame(dt)
    -- Advance the animation timer.
    elapsed = elapsed + dt
    if elapsed >= 1 / fps then
        elapsed = elapsed - 1 / fps
        -- Wrap back to 1 after the last frame (frame indices are 1-based).
        frame = frame % sheet:frame_count() + 1
    end

    clear(0.05, 0.05, 0.05, 1)

    -- Draw the current frame centred and scaled up.
    local dw = frame_w * scale
    local dh = frame_h * scale
    sheet:draw(frame,
               screen_width  / 2 - dw / 2,
               screen_height / 2 - dh / 2,
               dw, dh)

    -- HUD: frame counter in the top-left via coloured dots (no text API yet).
    -- Each dot represents one frame; the current frame dot is brighter.
    local dot = 8
    local pad = 4
    for i = 1, sheet:frame_count() do
        local bright = (i == frame) and 1.0 or 0.25
        set_color(bright, bright, bright, 1)
        draw_rect(pad + (i - 1) * (dot + 2), pad, dot, dot)
    end
end
