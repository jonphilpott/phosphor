-- scenes/test.lua
-- Phase 4 acceptance test: exercises all drawing primitives and the
-- matrix stack.  You should see:
--   - Black background
--   - A white rect in the top-left quadrant
--   - A red circle in the centre
--   - A green diagonal line across the screen
--   - A rotating yellow square in the bottom-right, driven by time
--   - White dots scattered in a grid pattern

local t = 0

function on_load()
    print("test.lua — screen " .. screen_width .. "x" .. screen_height)
end

function on_frame(dt)
    t = t + dt

    local W = screen_width
    local H = screen_height

    -- Black background
    clear(0, 0, 0, 1)

    -- Static white rect, top-left area
    set_color(1, 1, 1, 1)
    draw_rect(40, 40, W * 0.2, H * 0.2)

    -- Red filled circle at screen centre
    push()
      translate(W*0.5, H*0.5)
      set_color(1, 0, 0, 1)
      scale(t)
      draw_circle(0, 0, 50)
    pop()

    -- Green diagonal line, stroke weight 3
    set_stroke(0, 1, 0, 1)
    set_stroke_weight(3)
    draw_line(0, 0, W, H)

    -- Yellow square in bottom-right corner, rotating over time
    push()
        translate(W * 0.75, H * 0.75)
        rotate(t)
        set_color(0, 0, 1, 0.5)
	scale(t)
        draw_rect(-50, -50, 100, 100)
    pop()

    push()
        translate(W * 0.25, H * 0.25)
        rotate(t)
        set_color(0, 1, 0, 0.5)
	scale(t)
        draw_rect(-50, -50, 100, 100)
    pop()

    -- Grid of white dots
    set_stroke(1, 1, 1, 0.6)
    set_stroke_weight(4)
    local spacing = 60
    for gx = spacing, W - spacing, spacing do
        for gy = spacing, H - spacing, spacing do
            draw_point(gx, gy)
        end
    end

    shader_set("scanlines", "chromatic_ab")
end
