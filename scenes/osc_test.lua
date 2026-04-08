-- scenes/osc_test.lua
-- Phase 10 acceptance test: OSC → Lua dispatch.
--
-- Listens on UDP port 9000 (the engine default).
--
-- Supported addresses:
--   /color  r g b     — set background colour (floats 0.0–1.0)
--   /size   n         — set the dot radius in pixels
--   /bang             — flash the screen white for one frame
--
-- Test from SuperCollider:
--
--   ~phosphor = NetAddr("127.0.0.1", 9000);
--   ~phosphor.sendMsg("/color", 1.0, 0.0, 0.5);
--   ~phosphor.sendMsg("/size",  120.0);
--   ~phosphor.sendMsg("/bang");
--
-- Or cycle through colours with a Routine:
--
--   (
--   ~colors = [[1.0, 0.0, 0.4], [0.0, 1.0, 0.4], [0.0, 0.4, 1.0]];
--   r = Routine({
--       loop {
--           ~colors.do { |c|
--               ~phosphor.sendMsg("/color", c[0], c[1], c[2]);
--               0.5.wait;
--           };
--       };
--   }).play(AppClock);
--   )
--   r.stop;

local bg_r,  bg_g,  bg_b  = 0.05, 0.05, 0.05
local dot_r, dot_g, dot_b = 0.0,  1.0,  0.4
local dot_size = 60
local flash    = false

function on_osc(addr, ...)
    local args = {...}
    if addr == "/color" then
        bg_r = args[1] or bg_r
        bg_g = args[2] or bg_g
        bg_b = args[3] or bg_b
    elseif addr == "/size" then
        dot_size = args[1] or dot_size
    elseif addr == "/bang" then
        flash = true
    end
    -- Unknown addresses are silently ignored.
end

function on_frame(dt)
    if flash then
        clear(1, 1, 1, 1)
        flash = false
    else
        clear(bg_r, bg_g, bg_b, 1)
    end

    -- Pulsing dot at screen centre — size controlled by /size, colour fixed.
    set_color(dot_r, dot_g, dot_b, 1)
    set_circle_segments(64)
    draw_circle(screen_width / 2, screen_height / 2, dot_size)
end
