-- scenes/easing_demo.lua
-- A living showcase of all seven easing primitives built into the engine:
--   lerp, smooth, smooth_hl, map, clamp, smoothstep, pulse
--
-- Four coloured dots chase a drifting target at different smoothing rates.
-- Watching them diverge and converge makes the difference between the
-- rate-based and half-life exponential smoothers immediately visible.
-- A beat pulse colours the background.  smoothstep and lerp control
-- dot size and opacity based on how far each dot is from its target.
--
-- OSC (port 9000):
--   /target  x y  -- target position, normalised 0..1 (mapped to screen pixels)
--   /bpm     f    -- beat rate for the background flash (default 120)
--   /trail   f    -- feedback persistence alpha 0..1 (default 0.65)
--
-- SuperCollider:
--   ~p = NetAddr("127.0.0.1", 9000);
--   ~p.sendMsg("/target", 0.75, 0.3);
--   ~p.sendMsg("/bpm", 140.0);

local t          = 0.0
local bpm        = 120.0
local trail      = 0.65
local tgt_x      = 0.0
local tgt_y      = 0.0
local osc_tgt    = false   -- true while OSC is driving the target
local ready      = false   -- deferred init flag; screen_width isn't valid until on_frame

-- ── Follower definitions ──────────────────────────────────────────────────────
-- Each entry carries its current pixel position and the smoothing parameter.
-- rate ~= nil  → uses smooth()   (exponential, rate-based)
-- rate == nil  → uses smooth_hl() (exponential, half-life-based; see on_frame)
--
-- Choosing rate vs. half_life:
--   rate  is intuitive for control-rate thinking: "63% closed per second".
--   half_life is intuitive for musical time: "gap halves every N seconds".
local followers = {
    { x=0, y=0, rate=18,  r=1.0, g=0.3, b=0.3 },  -- snappy red   (rate-based)
    { x=0, y=0, rate=6,   r=0.3, g=1.0, b=0.4 },  -- medium green (rate-based)
    { x=0, y=0, rate=1.8, r=0.3, g=0.6, b=1.0 },  -- lazy blue    (rate-based)
    { x=0, y=0, rate=nil, r=1.0, g=0.9, b=0.3 },  -- yellow: half-life = 0.25s
}

-- ── OSC ───────────────────────────────────────────────────────────────────────

function on_osc(addr, ...)
    local args = {...}
    if addr == "/target" then
        -- map() converts the OSC normalised 0..1 values into pixel coordinates.
        -- This decouples the scene from any fixed resolution.
        tgt_x   = map(args[1] or 0.5, 0, 1, 0, screen_width)
        tgt_y   = map(args[2] or 0.5, 0, 1, 0, screen_height)
        osc_tgt = true
    elseif addr == "/bpm" then
        bpm = clamp(args[1] or bpm, 20, 300)
    elseif addr == "/trail" then
        trail = clamp(args[1] or trail, 0, 0.99)
    end
end

-- ── Main loop ─────────────────────────────────────────────────────────────────

function on_frame(dt)
    t = t + dt

    -- Deferred init: screen_width / screen_height are not valid until on_frame.
    -- We place all followers at the screen centre so the snappy red dot arrives
    -- almost immediately; the lazy blue one drifts in over the first few seconds.
    if not ready then
        local cx = screen_width  * 0.5
        local cy = screen_height * 0.5
        tgt_x, tgt_y = cx, cy
        for _, f in ipairs(followers) do
            f.x, f.y = cx, cy
        end
        ready = true
    end

    -- ── Auto-drift target when OSC isn't driving it ───────────────────────────
    -- Two overlapping sines at incommensurable frequencies give a Lissajous-like
    -- path that never perfectly repeats.
    if not osc_tgt then
        -- map() converts the [-1,1] sine output into the inner 70% of the screen.
        tgt_x = map(math.sin(t * 0.41),
                    -1, 1,
                    screen_width  * 0.15, screen_width  * 0.85)
        tgt_y = map(math.sin(t * 0.29),
                    -1, 1,
                    screen_height * 0.15, screen_height * 0.85)
    end

    -- ── Beat pulse ────────────────────────────────────────────────────────────
    -- pulse() returns 0 between beats and rises to 1 exactly at the downbeat,
    -- smoothly fading back to 0 over the 'width' window (here 70ms).
    -- We scale it to a gentle warm glow rather than a hard white strobe.
    local flash = pulse(t, bpm, 0.07)
    local bg    = flash * 0.10
    clear(bg, bg * 0.7, bg * 0.3, 1)
    draw_feedback(trail)

    -- ── Followers ─────────────────────────────────────────────────────────────
    for _, f in ipairs(followers) do

        -- Step 1: advance the follower toward the target this frame.
        if f.rate then
            -- smooth(): each frame the remaining gap is multiplied by e^(-rate*dt).
            -- Frame-rate independent — a gap of 100px with rate=6 closes at the
            -- same speed at 30fps as at 120fps.
            f.x = smooth(f.x, tgt_x, f.rate, dt)
            f.y = smooth(f.y, tgt_y, f.rate, dt)
        else
            -- smooth_hl(): same maths, but expressed as "gap halves every N seconds".
            -- At half_life=0.25 the yellow dot covers 75% of any gap in half a second.
            f.x = smooth_hl(f.x, tgt_x, 0.25, dt)
            f.y = smooth_hl(f.y, tgt_y, 0.25, dt)
        end

        -- Step 2: how far is this dot from the target (in pixels)?
        local dx   = f.x - tgt_x
        local dy   = f.y - tgt_y
        local dist = math.sqrt(dx*dx + dy*dy)

        -- map() normalises dist from pixel-space into [0,1].
        -- 500px is the "full speed" reference; anything beyond that is clamped to 1.
        -- clamp() prevents dist_frac going above 1 when the dot is very far away.
        local dist_frac = clamp(map(dist, 0, 500, 0, 1), 0, 1)

        -- Step 3: size and opacity.
        -- smoothstep() gives an S-curve on dist_frac so the dot grows slowly at
        -- first (barely moving), accelerates in the middle, then softens as it
        -- nears its full size.  This feels more organic than a linear ramp.
        local sz    = lerp(7, 38, smoothstep(dist_frac))

        -- lerp() blends opacity — full brightness when distant, dimmer when resting.
        local alpha = lerp(0.45, 1.0, dist_frac)

        set_color(f.r * alpha, f.g * alpha, f.b * alpha, 1)
        set_circle_segments(32)
        draw_circle(f.x, f.y, sz)
    end

    -- ── Target crosshair ──────────────────────────────────────────────────────
    -- A small white cross marks the destination — without it you'd have to infer
    -- the target from where the snappy red dot is pointing.
    local arm = 14
    set_stroke(1, 1, 1, 0.65)
    set_stroke_weight(1.5)
    draw_line(tgt_x - arm, tgt_y,       tgt_x + arm, tgt_y)
    draw_line(tgt_x,       tgt_y - arm, tgt_x,       tgt_y + arm)
end
