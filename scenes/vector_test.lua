-- scenes/vector_test.lua
-- The 2D vector type, and state that survives a hot reload.
--
-- What to look at
-- ──────────────
--   1. A field of particles steering toward a drifting attractor.
--   2. The attractor itself chased with vec:smooth_() — frame-rate-independent
--      following, which is the function you will reach for most often.
--   3. Both readable (operator) and allocation-free (in-place) vector styles,
--      side by side, doing the same kind of work.
--   4. Colour from HSV, hue driven by particle speed.
--
-- ── Try this while it runs ───────────────────────────────────────────────────
-- Edit any number below and save. The particles do NOT restart: they are held
-- in `persist`, so they keep flying with their current positions and velocities
-- while the code around them is replaced. Change TRAIL to 0.75, change the hue
-- range, change FORCE — the field reacts mid-flight.
--
-- To reset the field instead, delete the `persist.particles` line, save, put it
-- back, and save again. Or just press R twice.

local COUNT     = 300     -- particles
local FORCE     = 900     -- steering strength, pixels/sec^2
local MAX_SPEED = 260     -- pixels/sec
local TRAIL     = 0.90    -- feedback: higher = longer phosphor trail

function on_load()
    shader_set("scanlines")

    -- Build the field only if we do not already have one.
    --
    -- This `or` idiom is the whole trick to persist: on a first run the field is
    -- nil so we create it; on every reload after that it already exists and the
    -- constructor is skipped, so the simulation continues uninterrupted.
    if not persist.particles then
        persist.particles = {}
        for i = 1, COUNT do
            persist.particles[i] = {
                pos = vec(math.random() * screen_width,
                          math.random() * screen_height),
                -- vec.random(len) gives a uniformly distributed direction,
                -- scaled to the length you ask for.
                vel = vec.random(math.random() * 60 + 20),
            }
        end
    end

    -- Scalars and vectors both persist, so the clock survives too.
    persist.t     = persist.t     or 0
    persist.chase = persist.chase or vec(screen_width / 2, screen_height / 2)
    persist.loads = (persist.loads or 0) + 1
end

function on_frame(dt)
    persist.t = persist.t + dt
    local t = persist.t

    -- Phosphor trail: blend the previous frame back in rather than clearing.
    draw_feedback(TRAIL)

    -- Light adds up where particles overlap, instead of the topmost one
    -- winning. This is what makes the dense areas glow.
    set_blend("add")

    -- ── The attractor ────────────────────────────────────────────────────────
    -- A target position wandering on two independent noise fields, and a second
    -- vector chasing it.
    --
    -- vec:smooth_(target, rate, dt) closes the gap by a fixed *proportion* per
    -- second rather than a fixed step per frame, so it behaves identically at
    -- 30fps and 144fps. Raise the rate for a snappier chase; lower it for drift.
    local target_x = (noise(t * 0.15) * 0.5 + 0.5) * screen_width
    local target_y = (noise(t * 0.15 + 100) * 0.5 + 0.5) * screen_height
    persist.chase:smooth_(target_x, target_y, 1.5, dt)

    local chase = persist.chase

    -- ── The particles: in-place style ────────────────────────────────────────
    -- Every operator (a + b, v * 2) builds a NEW vector. That reads beautifully
    -- and costs nothing noticeable for a handful of vectors — but this loop runs
    -- 300 times a frame, 60 times a second, and that much garbage eventually
    -- shows up as a stutter when the collector catches up.
    --
    -- So the hot loop uses the trailing-underscore methods, which modify the
    -- vector in place and allocate nothing. One scratch vector is reused for
    -- every particle.
    local steer = vec()

    for i = 1, #persist.particles do
        local p = persist.particles[i]

        -- steer = normalize(chase - pos) * FORCE * dt, without allocating:
        steer:set_(chase):sub_(p.pos):normalize_():scale_(FORCE * dt)

        p.vel:add_(steer):limit_(MAX_SPEED)

        -- Vector methods also accept two plain numbers, which saves building a
        -- temporary just to scale velocity by dt.
        p.pos:add_(p.vel.x * dt, p.vel.y * dt)

        -- Wrap at the screen edges.
        if p.pos.x < 0 then p.pos.x = screen_width  end
        if p.pos.x > screen_width then p.pos.x = 0  end
        if p.pos.y < 0 then p.pos.y = screen_height end
        if p.pos.y > screen_height then p.pos.y = 0 end

        -- Hue from speed: slow particles cyan, fast ones magenta.
        -- HSV components are all 0..1 here, and hue wraps, so you can feed it
        -- anything without clamping.
        local speed = p.vel:mag()
        set_stroke_hsv(0.5 + (speed / MAX_SPEED) * 0.35, 0.9, 1.0, 0.9)
        set_stroke_weight(1.5)

        -- A short streak pointing back along the direction of travel.
        draw_line(p.pos.x, p.pos.y,
                  p.pos.x - p.vel.x * 0.045,
                  p.pos.y - p.vel.y * 0.045)
    end

    -- ── A ring of markers: operator style ────────────────────────────────────
    -- Only twelve of them, so readability wins over allocation here. This is the
    -- form to write by default.
    local radius = 40 + math.sin(t * 2) * 12
    for i = 0, 11 do
        local angle = (i / 12) * math.pi * 2 + t * 0.6

        -- from_angle gives a unit vector; scale it and offset it to the chase
        -- position with ordinary arithmetic.
        local point = chase + vec.from_angle(angle) * radius

        -- Fade the markers by how far the ring has swung from the attractor.
        set_color_hsv(0.08, 0.8, 1.0, 0.5)
        draw_circle(point.x, point.y, 2.5)
    end

    -- The attractor itself.
    set_color_hsv(0.12, 0.35, 1.0, 0.8)
    draw_circle(chase.x, chase.y, 5)

    -- ── Proof that persist works ─────────────────────────────────────────────
    -- Back to normal blending so the text is legible over the glow.
    set_blend("alpha")
    set_color(0.55, 0.75, 0.7, 1)
    draw_text(16, 16, string.format(
        "loads: %d   elapsed: %.1fs   particles: %d\nedit and save - the field keeps flying",
        persist.loads, t, #persist.particles), 2)
end
