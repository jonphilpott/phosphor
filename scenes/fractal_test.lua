-- fractal_test.lua
-- Demonstrates the Mandelbrot and Julia set shaders as full-screen post-process
-- passes.  The fractals run through shader_set_uniform so custom uniforms
-- (zoom, center, c value, etc.) are fully controllable.
--
-- NOTE: fractal shaders ignore u_texture — they generate the image themselves.
-- Any geometry drawn before end_frame is invisible under the fractal.  That is
-- intentional here: the fractal IS the scene.  To composite geometry on top,
-- draw it after the canvas:draw() call in a setup where the fractal is rendered
-- into a canvas — but that requires canvas uniform support (not yet implemented).
--
-- Behaviour:
--   Phases alternate every ~20 seconds:
--     Phase 0  — Mandelbrot, slowly zooming toward the boundary at (-0.75, 0.1)
--     Phase 1  — Julia, c orbiting automatically; julia_cx/cy settable via OSC
--     Phase 2  — Julia "pinned" to the Douady rabbit (c = -0.75, 0.11)
--     Phase 3  — Mandelbrot, zoomed into a deep spiral near (-0.16, 1.03)
--
-- OSC:
--   /zoom      f  — override zoom for current fractal (reset on phase change)
--   /max_iter  f  — iteration cap, default 128 (higher = slower but sharper)
--   /speed     f  — phase advance speed, default 1
--   /julia_cx  f  — pin Julia c real part (disables auto-orbit)
--   /julia_cy  f  — pin Julia c imaginary part

local t       = 0.0
local speed   = 1.0
local max_iter = 128.0
local zoom_override = nil   -- set by OSC /zoom; nil = use per-phase default
local julia_cx_pin  = nil   -- set by OSC; nil = auto-orbit
local julia_cy_pin  = nil

-- Phase definitions: each entry drives one ~20-second segment.
-- Fields: shader, cx, cy, zoom_start, zoom_end, animate, color_speed
local phases = {
    {   -- Mandelbrot zoom toward seahorse valley
        shader      = "mandelbrot",
        cx          = -0.75,  cy = 0.1,
        zoom_start  = 1.0,    zoom_end = 6.0,
        color_speed = 0.04,
    },
    {   -- Julia, c auto-orbiting at radius 0.35
        shader      = "julia",
        cx          = -0.7,   cy = 0.27,
        zoom_start  = 1.2,    zoom_end = 1.2,
        animate     = 0.35,
        color_speed = 0.05,
    },
    {   -- Julia: Douady rabbit (fixed c)
        shader      = "julia",
        cx          = -0.75,  cy = 0.11,
        zoom_start  = 1.0,    zoom_end = 1.8,
        animate     = 0.0,
        color_speed = 0.03,
    },
    {   -- Mandelbrot deep zoom near the boundary
        shader      = "mandelbrot",
        cx          = -0.16,  cy = 1.03,
        zoom_start  = 1.0,    zoom_end = 12.0,
        color_speed = 0.06,
    },
}

local phase_duration = 20.0   -- seconds per phase
local n_phases       = #phases

function on_load()
    -- nothing to preload; shaders are loaded from disk by shader_set()
end

function on_osc(addr, ...)
    local args = {...}
    if addr == "/zoom"     then zoom_override = args[1] end
    if addr == "/max_iter" then max_iter      = args[1] end
    if addr == "/speed"    then speed         = args[1] end
    if addr == "/julia_cx" then julia_cx_pin  = args[1] end
    if addr == "/julia_cy" then julia_cy_pin  = args[1] end
    -- /reset clears overrides and lets the auto-sequence resume
    if addr == "/reset" then
        zoom_override = nil
        julia_cx_pin  = nil
        julia_cy_pin  = nil
    end
end

function on_frame(dt)
    t = t + dt * speed

    -- Work out which phase we're in and how far through it (0..1).
    local total    = t % (phase_duration * n_phases)
    local phase_i  = math.floor(total / phase_duration) + 1
    local frac     = (total % phase_duration) / phase_duration
    local p        = phases[phase_i]

    -- Zoom lerps from zoom_start to zoom_end over the phase.
    local zoom = zoom_override
              or (p.zoom_start + (p.zoom_end - p.zoom_start) * frac)

    -- Color shift drifts continuously across phases so there's no seam.
    local color_shift = t * (p.color_speed or 0.04)

    -- ── Activate the fractal shader ───────────────────────────────────────────
    -- shader_set() replaces the full post-process pipeline for this frame.
    -- Uniforms must be set AFTER shader_set() so the shader program exists.
    shader_set(p.shader, "chromatic_ab", "scanlines")

    if p.shader == "mandelbrot" then
        shader_set_uniform("u_center_x",    p.cx)
        shader_set_uniform("u_center_y",    p.cy)
        shader_set_uniform("u_zoom",        zoom)
        shader_set_uniform("u_max_iter",    max_iter)
        shader_set_uniform("u_color_shift", color_shift)
        shader_set_uniform("u_color_speed", 0.0)  -- driven manually above
    else
        -- Julia: allow OSC to pin c, otherwise use phase defaults + shader orbit
        local cx = julia_cx_pin or p.cx
        local cy = julia_cy_pin or p.cy
        shader_set_uniform("u_julia_cx",    cx)
        shader_set_uniform("u_julia_cy",    cy)
        shader_set_uniform("u_zoom",        zoom)
        shader_set_uniform("u_max_iter",    max_iter)
        shader_set_uniform("u_color_shift", color_shift)
        shader_set_uniform("u_color_speed", 0.0)
        shader_set_uniform("u_animate",     julia_cx_pin and 0.0 or (p.animate or 0.0))
    end

    -- The scene is intentionally empty — the fractal shader generates the full
    -- image from u_time and the uniforms above.  clear() is still called so the
    -- FBO is in a defined state before the post-process pass.
    clear(0, 0, 0, 1)
end
