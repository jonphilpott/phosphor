-- scenes/conway_test.lua
-- Phase 6 acceptance test: Conway's Game of Life.
--
-- The grid fills the screen at 4×4 pixel cells.  Press nothing — it just runs.
-- A random soup at ~35% density usually settles into a mix of still lifes,
-- oscillators, and gliders within a few hundred generations.

local cell_w = 4
local cell_h = 4

local cols = math.floor(screen_width  / cell_w)
local rows = math.floor(screen_height / cell_h)

local g = conway.new(cols, rows)
g:randomize(0.35)

function on_frame(dt)
    clear(0, 0, 0, 1)
    g:step()
    set_color(0, 1, 0.4, 1)   -- green-ish phosphor glow
    g:draw(0, 0, cell_w, cell_h)
end
