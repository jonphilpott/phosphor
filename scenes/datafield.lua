-- scenes/datafield.lua
-- Precision data aesthetics: monochrome geometry, scan pulse, noise-driven fields.
--
-- Visual layers (all monochrome — pure white on black):
--   1. Stochastic dot grid  — sparse grid, noise-modulated, OSC density control
--   2. Data stream rows     — horizontal dot rows displaced by scrolling noise
--   3. Barcode strip        — vertical bars of varying width across the bottom
--   4. Scan pulse           — bright line sweeping top→bottom like a CRT raster
--
-- OSC control (port 9000):
--   /speed    f   — animation speed multiplier (default 1.0)
--   /density  f   — dot grid fill 0..1 (default 0.5)
--   /pulse    f   — scan line brightness 0..1 (default 1.0)
--
-- SuperCollider:
--
--   ~p = NetAddr("127.0.0.1", 9000);
--   ~p.sendMsg("/speed",   2.0);
--   ~p.sendMsg("/density", 0.8);
--   ~p.sendMsg("/pulse",   0.6);
--
-- Or live-automate with a Routine:
--
--   (
--   r = Routine({
--       loop {
--           ~p.sendMsg("/speed",   rrand(0.5, 3.0));
--           ~p.sendMsg("/density", rrand(0.2, 0.9));
--           1.5.wait;
--       };
--   }).play(AppClock);
--   )
--   r.stop;

local speed   = 1.0   -- multiplies dt so everything slows/speeds uniformly
local density = 0.5   -- fraction of grid dots that are visible (0=none, 1=all)
local pulse_b = 1.0   -- brightness of the horizontal scan pulse

local t = 0.0         -- master time accumulator (seconds × speed)

function on_load()
    -- Scanlines give the alternating-row dimming that Ikeda's work often has
    -- when screenshotted from CRT output.  Adds texture without geometry cost.
    shader_set("glitch", "chromatic_ab")
end

function on_osc(addr, ...)
    local args = {...}
    if addr == "/speed"   then speed   = args[1] or speed   end
    if addr == "/density" then density = args[1] or density end
    if addr == "/pulse"   then pulse_b = args[1] or pulse_b end
end

-- ── helper: barcode strip ────────────────────────────────────────────────────
-- Fills the bottom 18% of the screen with vertical bars.
-- Bar widths and gaps are seeded from noise so they look data-derived but
-- stay stable frame-to-frame (noise is position-keyed, not time-keyed here).
local function draw_barcode()
    local strip_h = math.floor(screen_height * 0.18)
    local y0      = screen_height - strip_h
    local x       = 0
    while x < screen_width do
        -- Width 1–9 px driven by noise at this x position
        local w = math.max(1, math.floor((noise(x * 0.02) * 0.5 + 0.5) * 8 + 1))
        -- Subtle brightness flicker over time — bars breathe slightly
        local b = 0.55 + 0.45 * (noise(x * 0.015, t * 0.25) * 0.5 + 0.5)
        set_color(b, b, b, 1)
        draw_rect(x, y0, w, strip_h)
        -- Gap between bars, also noise-derived
        local gap = math.max(1, math.floor((noise(x * 0.02 + 100) * 0.5 + 0.5) * 3 + 1))
        x = x + w + gap
    end
end

-- ── helper: data stream rows ─────────────────────────────────────────────────
-- Horizontal rows of dots whose vertical position oscillates with scrolling
-- Perlin noise — creates the impression of waveforms or data bus signals.
-- The upper 55% of the screen is used so the barcode strip stays separate.
local function draw_streams()
    local num_rows = 8
    local region_h = screen_height * 0.52
    local row_h    = region_h / num_rows
    set_stroke_weight(2)

    for row = 0, num_rows - 1 do
        local base_y = 28 + row * row_h
        for x = 0, screen_width - 1, 4 do
            -- Noise scrolls left at a row-dependent rate → rows look independent
            local scroll = t * (0.35 + row * 0.06)
            local n      = noise(x * 0.007 + scroll, row * 1.9)
            local dy     = n * (row_h * 0.38)

            -- Brightness hotspots move slowly so attention sweeps naturally
            local bright = noise(x * 0.005 - t * 0.15, row * 2.1) * 0.5 + 0.5
            bright = math.max(0.15, bright)
            set_stroke(bright, bright, bright, 1)
            draw_point(x, base_y + dy)
        end
    end
end

-- ── helper: stochastic dot grid ──────────────────────────────────────────────
-- Covers the same upper region as the streams; drawn first so streams sit on
-- top.  Each grid position is only drawn when noise exceeds a threshold that
-- the user can shift with OSC /density.
local function draw_dot_grid()
    local step = 12
    set_stroke_weight(1.5)
    -- Map density 0..1 → threshold -0.5..0.5
    -- density=1 means threshold=-0.5 → nearly all points drawn (noise rarely < -0.5)
    -- density=0 means threshold= 0.5 → almost nothing drawn
    local threshold = 0.5 - density

    for y = 0, screen_height * 0.75, step do
        for x = 0, screen_width - 1, step do
            -- Two-axis scroll: grid appears to drift diagonally
            local n = noise(x * 0.025 + t * 0.08, y * 0.025 - t * 0.055)
            if n > threshold then
                -- Fade brightness near the threshold so edges aren't hard
                local bright = math.min(1.0, (n - threshold) * 3.0)
                set_stroke(bright, bright, bright, bright)
                draw_point(x, y)
            end
        end
    end
end

-- ── helper: scan pulse ───────────────────────────────────────────────────────
-- A bright band sweeps from top to bottom every 3 seconds (adjusted by speed).
-- Drawn as a stack of horizontal lines with alpha falloff from the centre —
-- like a CRT's electron beam, or the shutter of a camera panning a monitor.
local function draw_scan_pulse()
    if pulse_b <= 0.01 then return end
    local period = 3.0 / speed
    local frac   = (t / period) % 1.0     -- 0..1 → fraction of screen height
    local y      = math.floor(frac * screen_height)

    -- The pulse has a soft edge: ±4 pixels with quadratic falloff
    for dy = -4, 4 do
        local alpha = (1.0 - (dy * dy) / 16.0) * pulse_b
        set_stroke(1, 1, 1, alpha)
        set_stroke_weight(1)
        draw_line(0, y + dy, screen_width, y + dy)
    end
end

-- ── on_frame ─────────────────────────────────────────────────────────────────

function on_frame(dt)
    t = t + dt * speed

    clear(0, 0, 0, 1)

    -- A light feedback ghost extends the scan pulse's trail slightly — just
    -- enough to give it a phosphor afterglow without muddying the geometry.
    draw_feedback(0.10, 1.0, 0.0)

    -- Draw order: grid behind streams behind barcode behind pulse
    draw_dot_grid()
    draw_streams()
    draw_barcode()
    draw_scan_pulse()
end
