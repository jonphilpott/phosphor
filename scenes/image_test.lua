-- scenes/image_test.lua
-- Phase 7 acceptance test: single image loading and sub-region drawing.
--
-- Expects:  assets/test.png  (any PNG will do — drop one in the assets/ folder)
--
-- Demonstrates three draw modes:
--   1. Full image at natural pixel size (top-left of screen)
--   2. Full image scaled to half size (centred)
--   3. Top-left quarter of the image drawn at natural size (top-right corner)

local img   -- populated in on_load()

function on_load()
    img = image.load("assets/test.png")
end

function on_frame(dt)
    clear(0.05, 0.05, 0.05, 1)

    -- 1. Full image at natural size, anchored at top-left.
    img:draw(0, 0)

    -- 2. Full image at half size, centred on screen.
    local hw = img:width()  / 2
    local hh = img:height() / 2
    img:draw(screen_width / 2 - hw / 2,
             screen_height / 2 - hh / 2,
             hw, hh)

    -- 3. Top-left quarter drawn at natural size in the top-right corner.
    --    draw_region(dst_x, dst_y, dst_w, dst_h,  src_x, src_y, src_w, src_h)
    local qw = img:width()  / 2
    local qh = img:height() / 2
    img:draw_region(screen_width - qw, 0,   -- destination position
                    qw, qh,                 -- destination size
                    0, 0, qw, qh)           -- source sub-rect (top-left quarter)
end
