-- scenes/wolfram_test.lua
-- Phase 6 acceptance test: Wolfram 1D elementary cellular automaton.
--
-- The automaton scrolls upward so the oldest generation is at the top and
-- the newest at the bottom.  Cell size is chosen so the full width fills the
-- screen and the history fills the screen height.
--
-- Rule 30  → chaotic / noise-like (used in Mathematica's random number generator)
-- Rule 90  → Sierpinski triangle (self-similar fractal)
-- Rule 110 → Turing-complete complex structure
-- Rule 184 → traffic flow simulation
--
-- Change `rule` below and relaunch to compare patterns.

local rule   = 90
local cell_w = 8
local cell_h = 8

local cols = math.floor(screen_width  / cell_w)
local rows = math.floor(screen_height / cell_h)

local ca = wolfram.new(rule, cols, rows)
ca:seed()   -- single centre cell, classic triangle starting condition

function on_frame(dt)
    clear(0, 0, 0, 1)
    ca:step()
    set_color(1, 1, 1, 1)
    ca:draw(0, 0, cell_w, cell_h)
end
