-- scenes/lissajous.lua
-- Parametric Lissajous curves with 3D projection and feedback trail.
--
-- Classic 2D Lissajous: x = sin(a·t + δ),  y = sin(b·t)
-- Extended to 3D:        z = sin(c·t)       (c=0 gives flat 2D figure)
--
-- 512 sample points are drawn as a connected polyline each frame.
-- draw_feedback() provides the neon persistence trail — no history buffer
-- needed because the FBO carries the previous frame forward automatically.
--
-- OSC control (port 9000):
--   /ratio  a  b   — frequency ratio (floats, e.g. 3 2 or 1.618 1)
--   /phase  f      — phase offset δ in radians between x and y (default 0)
--   /zfreq  f      — z-axis frequency; 0 = flat 2D (default 0)
--   /trail  f      — feedback alpha 0..1 (default 0.85)
--   /color  r g b  — curve colour as floats 0..1
--
-- SuperCollider:
--   ~p = NetAddr("127.0.0.1", 9000);
--   ~p.sendMsg("/ratio", 3.0, 2.0);
--   ~p.sendMsg("/phase", 0.5);
--   ~p.sendMsg("/zfreq", 1.3);   -- lifts to a 3D ribbon

local N_SAMPLES = 512   -- number of line segments per frame; more = smoother curves
local TWO_PI    = math.pi * 2

-- ── OSC-controlled parameters ─────────────────────────────────────────────────
local freq_a  = 3.0    -- x-axis frequency
local freq_b  = 2.0    -- y-axis frequency
local freq_z  = 0.0    -- z-axis frequency (0 = stays flat)
local phase_x = 0.0    -- phase offset between x and y (radians)
local trail   = 0.85   -- feedback alpha: higher = longer trail, lower = faster fade
local col_r, col_g, col_b = 0.2, 1.0, 0.5   -- default: phosphor green

-- t_offset drifts slowly over time.  When freq_a/freq_b is irrational, the
-- curve never perfectly repeats — this drift keeps it visibly evolving even
-- when ratios ARE rational (which would otherwise give a static closed figure).
local t_offset = 0.0

function on_load()
    math.randomseed(os.time())
    shader_set("glitch", "chromatic_ab")

    -- Place the camera along +Z looking back at the origin.
    -- This means our Lissajous coordinates (x,y in -1..1) map directly
    -- to screen space — intuitive and fills the frame at the default FOV.
    camera_3d(0, 0, 3, 0, 0, 0)
    perspective_3d(math.pi / 3)   -- 60° vertical FOV
end

function on_osc(addr, ...)
    local args = {...}
    if addr == "/ratio" then
        freq_a = args[1] or freq_a
        freq_b = args[2] or freq_b
    elseif addr == "/phase" then
        phase_x = args[1] or phase_x
    elseif addr == "/zfreq" then
        freq_z = args[1] or freq_z
    elseif addr == "/trail" then
        trail = args[1] or trail
    elseif addr == "/color" then
        col_r = args[1] or col_r
        col_g = args[2] or col_g
        col_b = args[3] or col_b
    end
end

function on_frame(dt)
    -- Drift the parametric offset — slow enough to be subliminal, fast enough
    -- to prevent any rational-ratio figure from staying perfectly frozen.
    t_offset = t_offset + dt * 4

    -- Clear to black then layer the previous frame at trail alpha.
    -- This is the "phosphor persistence" effect: old geometry fades out
    -- rather than vanishing instantly, giving the impression of a glowing screen.
    clear(0, 0, 0, 1)
    draw_feedback(trail)

    set_stroke(col_r, col_g, col_b, 1)
    set_stroke_weight(1.5)

    -- Walk N_SAMPLES+1 points around the parametric curve [0, 2π].
    -- We draw a line segment from point i-1 to point i, so we need the
    -- previous screen coordinate cached across iterations.
    local prev_sx, prev_sy

    for i = 0, N_SAMPLES do
        -- u is the parametric variable, one full cycle per frame.
        -- Adding t_offset makes the figure drift rather than stay locked.
        local u  = (i / N_SAMPLES) * TWO_PI + t_offset

        -- Standard Lissajous parametric equations.
        -- All three world coordinates are in [-1, 1] which fits neatly
        -- inside the camera frustum we set up in on_load.
        local wx = math.sin(freq_a * u + phase_x)
        local wy = math.sin(freq_b * u)
        local wz = math.sin(freq_z * u) * 0.4   -- scale z down so depth is subtle

        -- project_3d converts 3D world coordinates to 2D screen pixels.
        -- It returns nil,nil for points behind the camera (not possible here,
        -- but good practice to guard against it).
        local sx, sy = project_3d(wx, wy, wz)

        if sx and prev_sx then
            draw_line(prev_sx, prev_sy, sx, sy)
        end

        prev_sx, prev_sy = sx, sy
    end
end
