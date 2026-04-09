-- everything_test.lua
-- A combined demo of all four generator systems:
--
--   Layer 1 (background): Julia fractal rendered into a full-screen canvas.
--                         The fractal canvas is drawn at low alpha so it glows
--                         behind everything else without drowning it.
--
--   Layer 2 (mid):        Conway's Game of Life grid.  Cells are tinted by the
--                         fractal hue at their position — alive cells glow with
--                         the colour underneath them.
--
--   Layer 3 (foreground): 3D wireframe — a sphere and a cube orbit above the
--                         life grid.  The camera slowly circles the scene.
--
--   Layer 4 (HUD):        Waveform strip at the bottom of the screen.
--                         Population (live cell count / total cells) drives the
--                         amplitude of a sine wave in real time, so the waveform
--                         reacts to how busy the simulation is.
--
-- OSC:
--   /randomize  f  — re-seed life grid (f = density 0..1, default 0.35)
--   /speed      f  — life generation rate multiplier (default 1)
--   /julia_cx   f  — Julia c real part
--   /julia_cy   f  — Julia c imaginary part
--   /cam_dist   f  — camera orbit radius (default 7)

-- ── Config ────────────────────────────────────────────────────────────────────
local CELL      = 12          -- life grid cell size in pixels
local GEN_RATE  = 12          -- target generations per second
local WAVE_H    = 480         -- height of the waveform strip

-- ── State ─────────────────────────────────────────────────────────────────────
local t         = 0.0
local speed     = 1.0
local cam_dist  = 7.0
local julia_cx  = -0.7
local julia_cy  =  0.27

-- Life grid — the conway C++ primitive handles B3/S23 rule evaluation and
-- toroidal wrap.  We keep a Lua-side age[] table (flat, 1-indexed) for the
-- newborn colour fade, since conway only exposes binary live/dead state.
local ca                  -- conway userdata, created in on_load
local cols, rows
local age    = {}         -- age[r*cols + c + 1] = consecutive live generations

-- Generation accumulator: advance by whole steps, track fractional remainder.
local gen_acc   = 0.0
local pop       = 0.0    -- live cell fraction [0..1], smoothed

-- Canvas for the Julia background
local c_julia

-- ── Life helpers ──────────────────────────────────────────────────────────────

local function init_grid(density)
    density = density or 0.35
    cols = math.floor(screen_width  / CELL)
    rows = math.floor(screen_height / CELL)
    ca   = conway.new(cols, rows)
    ca:randomize(density)
    local n = cols * rows
    age = {}
    for i = 1, n do age[i] = 0 end
end

local function step_life()
    -- C++ evaluates one full B3/S23 generation (toroidal, double-buffered).
    ca:step()

    -- Walk every cell to update the age table and accumulate population.
    -- ca:get() is 1-indexed: get(col, row) with col and row both in [1, dim].
    local live = 0
    for r = 1, rows do
        for c = 1, cols do
            local i     = (r - 1) * cols + c
            local alive = ca:get(c, r)
            if alive == 1 then
                age[i] = age[i] + 1
                live   = live   + 1
            else
                age[i] = 0
            end
        end
    end

    -- One-pole lowpass keeps the population signal smooth so waveform
    -- amplitude doesn't jump on large die-off or birth events.
    pop = pop * 0.9 + (live / (cols * rows)) * 0.1
end

-- ── Lifecycle ─────────────────────────────────────────────────────────────────

function on_load()
    -- Lua 5.4: math.randomseed() with no args uses a time-based seed automatically.
    math.randomseed()
    init_grid(0.35)

    -- Create the Julia canvas sized to fill the screen.
    -- Fixed size at load time — resizes won't auto-update the canvas resolution,
    -- but it will be stretched to fit via canvas:draw().
    -- Use explicit integer cast — canvas.new requires integer dimensions.
    c_julia = canvas.new(math.floor(screen_width), math.floor(screen_height))
      shader_set("glitch", "chromatic_ab")
      -- dial the intensity up or down (default 0.35):
      shader_set_uniform("u_glitch_amount", 0.6)
end

function on_osc(addr, ...)
    local args = {...}
    if addr == "/randomize" then init_grid(args[1] or 0.35) end
    if addr == "/speed"     then speed    = args[1] end
    if addr == "/julia_cx"  then julia_cx = args[1] end
    if addr == "/julia_cy"  then julia_cy = args[1] end
    if addr == "/cam_dist"  then cam_dist = args[1] end
end

function on_frame(dt)
    -- Guard: if on_load failed (e.g. canvas FBO creation error), try once more.
    -- This surfaces the real error message rather than a confusing nil-index crash.
    if not c_julia then
        c_julia = canvas.new(math.floor(screen_width), math.floor(screen_height))
    end

    t = t + dt * speed
    clear(0.02, 0.02, 0.04, 1)

    -- ── Layer 1: Julia fractal background ────────────────────────────────────
    -- Render the Julia set into the canvas using set_uniform for full control.
    -- u_animate > 0 makes c orbit slowly, morphing the shape over time.
    c_julia:begin()
        clear(0, 0, 0, 1)
    c_julia:set_uniform("u_julia_cx",   julia_cx)
    c_julia:set_uniform("u_julia_cy",   julia_cy)
    c_julia:set_uniform("u_zoom",       1.3)
    c_julia:set_uniform("u_max_iter",   96.0)
    c_julia:set_uniform("u_color_shift", t * 0.02)
    c_julia:set_uniform("u_color_speed", 0.0)
    c_julia:set_uniform("u_animate",    0.25)   -- slow orbit of c
    c_julia:finish("julia")

    -- Draw fractal at low alpha so it glows behind the life grid.
    -- alpha is achieved by drawing a dark rect over it after — the fractal
    -- canvas always draws at full alpha, so we layer a semi-transparent veil.
    c_julia:draw(0, 0, screen_width, screen_height)

    -- Veil: darkens the fractal so life cells and waveforms are clearly readable.
    set_color(0.02, 0.02, 0.04, 0.72)
    draw_rect(0, 0, screen_width, screen_height)

    -- ── Advance the life simulation ───────────────────────────────────────────
    gen_acc = gen_acc + dt * GEN_RATE * speed
    local steps = math.min(math.floor(gen_acc), 4)  -- cap at 4 steps/frame
    gen_acc = gen_acc - steps
    for _ = 1, steps do step_life() end

    -- ── Layer 2: Conway's Game of Life grid ───────────────────────────────────
    -- Loop is 0-based for pixel maths; ca:get() takes 1-based col/row.
    for r = 0, rows - 1 do
        for c = 0, cols - 1 do
            if ca:get(c + 1, r + 1) == 1 then
                local i  = r * cols + c + 1
                -- Fade newborn cells (age 0) from white to base green.
                -- t_age approaches 1 after ~8 generations.
                local a  = math.min(age[i] / 8.0, 1.0)
                local rr = 1.0 - a * 0.7    -- white → green-ish
                local gg = 1.0
                local bb = 1.0 - a * 0.6
                set_color(rr, gg, bb, 0.9)
                draw_rect(c * CELL + 1, r * CELL + 1, CELL - 2, CELL - 2)
            end
        end
    end

    -- ── Layer 3: 3D wireframe ─────────────────────────────────────────────────
    -- Camera orbits the scene centre looking at the life grid from above-front.
    perspective_3d(1.047, 0.1, 200.0)
    local ex = math.cos(t * 0.25) * cam_dist
    local ez = math.sin(t * 0.25) * cam_dist
    camera_3d(ex, 4.0, ez,   0, 0, 0)

    -- Ghost grid on the floor plane (y=0), sized to approximately match the
    -- pixel grid above.  draw_wire_grid uses world units; we use 6 units wide.
    set_stroke(0.1, 0.4, 0.2, 1)
    set_stroke_weight(1)
    draw_wire_grid(6, 6, 0)

    -- Orbiting sphere — position traces a figure-8 (Lissajous) over the grid.
    set_stroke(0.3, 0.8, 1.0, 1)
    set_stroke_weight(1.2)
    local sx = math.cos(t * 0.4) * 2.0
    local sz = math.sin(t * 0.8) * 1.5
    draw_wire_sphere(sx, 1.5 + math.sin(t * 0.6) * 0.4, sz,  0.5,  8, 12)

    -- Spinning cube above centre — rotation driven by time on all three axes.
    set_stroke(1.0, 0.5, 0.2, 1)
    set_stroke_weight(1.4)
    draw_wire_cube(0, 2.5, 0,  0.7,  t * 0.9, t * 0.5, t * 0.3)

    -- ── Layer 4: Waveform strip (centred vertically) ─────────────────────────
    -- wy is the top of the strip, positioned so the block sits in the middle.
    local wy = math.floor((screen_height - WAVE_H) * 0.5)

    -- Four waveform channels, each occupying one quarter of the strip height.
    -- Channel 1: sine — frequency modulated by population (busier grid = faster)
    -- Channel 2: triangle — slow drift
    -- Channel 3: square
    -- Channel 4: sawtooth
    local ch_h = WAVE_H / 4
    local freq  = 1.0 + pop * 4.0   -- 1..5 cycles depending on how alive the grid is

    local channels = {
        { type = "sine",   phase = t * freq, r = 0.2, g = 0.9, b = 0.4 },
        { type = "tri",    phase = t * 0.7,  r = 0.3, g = 0.6, b = 1.0 },
        { type = "square", phase = t * 1.5,  r = 0.9, g = 0.4, b = 0.1 },
        { type = "saw",    phase = t * 2.0,  r = 0.8, g = 0.2, b = 0.7 },
    }

    set_stroke_weight(16.0)
    for i, ch in ipairs(channels) do
        set_stroke(ch.r, ch.g, ch.b, 0.9)
        draw_waveform(ch.type,
                      0, wy + (i-1) * ch_h,
                      screen_width, ch_h,
                      3.0, ch.phase)
    end

end
