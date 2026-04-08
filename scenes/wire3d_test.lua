-- wire3d_test.lua
-- Exercises the 3D wireframe API:
--
--   Top-left:    rotating wire cube
--   Top-right:   wire sphere (lat/lon rings)
--   Bottom:      wire grid as a floor plane
--   Centre:      project_3d() used to draw a manual point-cloud orbit
--
-- Camera slowly orbits around the scene so all faces are visible over time.
-- Press nothing — it's all automatic.
--
-- OSC:
--   /cam_dist  f   -- camera orbit radius (default 8)
--   /speed     f   -- rotation speed multiplier (default 1)

local t        = 0.0
local cam_dist = 8.0
local speed    = 1.0

function on_load()
    -- Set a perspective projection once; aspect is derived from screen size.
    -- FOV 60 degrees (1.047 rad), near=0.1, far=200.
    perspective_3d(1.047, 0.1, 200.0)
end

function on_osc(addr, ...)
    local args = {...}
    if addr == "/cam_dist" then cam_dist = args[1] end
    if addr == "/speed"    then speed    = args[1] end
end

function on_frame(dt)
    t = t + dt * speed
    clear(0.04, 0.04, 0.04, 1)

    -- ── Camera: orbit around the scene on a tilted circle ────────────────────
    -- eye traces a horizontal circle at height 3, radius cam_dist.
    -- target stays at the world origin.
    local eye_x = math.cos(t * 0.3) * cam_dist
    local eye_z = math.sin(t * 0.3) * cam_dist
    local eye_y = 3.0
    camera_3d(eye_x, eye_y, eye_z,   0, 0, 0)

    -- ── Floor grid ───────────────────────────────────────────────────────────
    -- 8-unit wide grid, 8 divisions, sitting at y=0.
    set_stroke(0.15, 0.25, 0.15, 1)
    set_stroke_weight(1)
    draw_wire_grid(8, 8, 0)

    -- ── Cube (top-left area of world, x=-2, y=1, z=-2) ───────────────────────
    -- Rotates independently on Y and X axes.
    set_stroke(0.3, 0.9, 0.4, 1)
    set_stroke_weight(1.5)
    draw_wire_cube(-2, 1, -2,   1.2,   t * 0.7, t * 0.4, 0)

    -- ── Sphere (top-right, x=2, y=1, z=-2) ───────────────────────────────────
    -- 10 latitude rings, 18 longitude arcs — smooth enough without being slow.
    set_stroke(0.3, 0.6, 1.0, 1)
    set_stroke_weight(1.2)
    draw_wire_sphere(2, 1, -2,   0.9,   10, 18)

    -- ── Point cloud: manual project_3d demo ──────────────────────────────────
    -- Orbit 24 points around a circle at y=1, drawn as screen-space dots.
    -- This tests that project_3d() returns sensible pixel coordinates and
    -- correctly returns nothing for points behind the camera.
    set_stroke(1.0, 0.7, 0.1, 1)
    set_stroke_weight(4)

    local pts = 24
    for i = 0, pts - 1 do
        local angle = (i / pts) * math.pi * 2 + t * 0.5
        local wx = math.cos(angle) * 1.5
        local wz = math.sin(angle) * 1.5
        local wy = 1.0 + math.sin(angle * 2 + t) * 0.4   -- slight vertical bob

        local sx, sy = project_3d(wx, wy, wz)
        if sx then
            -- Size the dot by "distance" — points near angle=0 are brighter.
            draw_point(sx, sy)
        end
    end

    -- ── Second cube: smaller, fast spin, sits above origin ───────────────────
    set_stroke(0.9, 0.3, 0.3, 1)
    set_stroke_weight(1)
    draw_wire_cube(0, 2.2, 0,   0.4,   t * 2.1, t * 1.3, t * 0.8)

    shader_set("scanlines", "chromatic_ab")
end
