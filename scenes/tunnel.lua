-- tunnel.lua
-- Classic demoscene dot tunnel.
--
-- A procedural path winds through 3D space, generating rings of dots.
-- The camera flies forward along the path. The path drifts organically
-- using overlapping sine waves; OSC impulses steer it hard.
--
-- Visual layers:
--   - Back-to-front ring draw order so near dots overdraw far ones
--   - Depth fade: far rings dimmer, near rings brighter
--   - Per-ring dot size gradient: large near, small far
--   - Helix phase offset so dots spiral instead of aligning in columns
--   - Subtle feedback trail for motion blur
--
-- OSC:
--   /color  r g b   -- dot colour (floats 0-1)
--   /speed  f       -- travel speed in rings/sec (default 2.5)
--   /left           -- impulse: steer tunnel left
--   /right          -- impulse: steer tunnel right
--   /up             -- impulse: steer tunnel up
--   /down           -- impulse: steer tunnel down

-- ── Config ────────────────────────────────────────────────────────────────────
local NUM_RINGS  = 48     -- rings drawn ahead of camera
local DOTS       = 22     -- dots per ring
local RING_R     = 1.5    -- tunnel radius (world units)
local RING_SPACE = 0.38   -- distance between rings (world units)
local HELIX      = 0.18   -- phase offset per ring so dots spiral (radians)

-- ── Parameters (OSC-controllable) ─────────────────────────────────────────────
local speed       = 2.5         -- rings per second
local cr, cg, cb  = 0.0, 1.0, 0.35  -- dot colour

-- ── Internal state ────────────────────────────────────────────────────────────
local t           = 0.0
local travel      = 0.0   -- fractional position between path[1] and path[2]
local ring_offset = 0     -- counts path_step() calls; keeps helix phase world-stable

-- Path buffer: NUM_RINGS visible rings + a few extra for lookahead/tangent
-- path[1]         = reference point just behind camera
-- path[2..N+1]    = visible rings
-- path[N+2..PBUF] = lookahead buffer for tangent + look-target
local PBUF = NUM_RINGS + 6
local path = {}   -- path[i] = {x, y, z}

-- Direction state for procedural path generation
local gen_yaw   = 0   -- heading: left/right angle (radians)
local gen_pitch = 0   -- heading: up/down angle (radians)
local yaw_v     = 0   -- yaw velocity (accumulates drift + impulses)
local pitch_v   = 0   -- pitch velocity

-- Impulses injected by OSC /left /right /up /down; decay each path step
local imp_yaw   = 0
local imp_pitch = 0

-- ── Math helpers ──────────────────────────────────────────────────────────────

local function norm3(x, y, z)
    local len = math.sqrt(x*x + y*y + z*z)
    if len < 1e-6 then return 0, 0, -1 end
    return x/len, y/len, z/len
end

local function cross3(ax, ay, az, bx, by, bz)
    return ay*bz - az*by,
           az*bx - ax*bz,
           ax*by - ay*bx
end

-- Given a forward tangent vector, return perpendicular right and up vectors.
-- Uses world-Y as the "up hint", falling back to world-X when heading near-vertical.
local function frame_from_tangent(tx, ty, tz)
    local hx, hy, hz = 0, 1, 0             -- world up hint
    if math.abs(ty) > 0.85 then
        hx, hy, hz = 1, 0, 0              -- swap to world X when near-vertical
    end
    -- right = cross(tangent, up_hint)  →  points to camera's right
    local rx, ry, rz = norm3(cross3(tx, ty, tz, hx, hy, hz))
    -- up   = cross(right, tangent)     →  recomputed true-up perpendicular to both
    local ux, uy, uz = norm3(cross3(rx, ry, rz, tx, ty, tz))
    return rx, ry, rz, ux, uy, uz
end

-- ── Path management ───────────────────────────────────────────────────────────

local function path_init()
    path = {}
    gen_yaw, gen_pitch = 0, 0
    yaw_v, pitch_v     = 0, 0
    imp_yaw, imp_pitch = 0, 0
    travel             = 0
    ring_offset        = 0

    -- Straight initial tunnel along -Z
    for i = 1, PBUF do
        path[i] = {0, 0, -(i - 1) * RING_SPACE}
    end
end

-- Advance path by one segment: drop path[1], extend with a new point at the tail.
-- Called whenever travel >= 1.0 (i.e. camera has crossed into the next segment).
local function path_step()
    -- ── Drift: two overlapping sine waves per axis give organic, non-repeating wander.
    -- Frequencies chosen to be irrational multiples so the pattern never repeats.
    local drift_yaw   = math.sin(t * 0.31) * 0.011
                      + math.sin(t * 0.17) * 0.007
    local drift_pitch = math.sin(t * 0.26) * 0.008
                      + math.cos(t * 0.43) * 0.005

    -- Accumulate velocity with damping + drift + OSC impulse.
    -- 0.93 damping keeps velocity from building indefinitely.
    yaw_v   = yaw_v   * 0.93 + drift_yaw   + imp_yaw
    pitch_v = pitch_v * 0.93 + drift_pitch + imp_pitch

    -- Decay impulses — they fade over several path steps rather than cutting off.
    imp_yaw   = imp_yaw   * 0.75
    imp_pitch = imp_pitch * 0.75

    gen_yaw   = gen_yaw + yaw_v
    gen_pitch = gen_pitch + pitch_v
    -- Soft bounce: when pitch hits the ceiling/floor, reverse and damp the
    -- velocity rather than hard-clamping it.  Hard clamping keeps pitch_v
    -- pushing against the wall every frame, causing a visible stutter.
    if gen_pitch >  0.75 then gen_pitch =  0.75; pitch_v = -pitch_v * 0.4 end
    if gen_pitch < -0.75 then gen_pitch = -0.75; pitch_v = -pitch_v * 0.4 end

    -- Convert yaw/pitch angles into a world-space direction vector.
    -- Standard spherical to Cartesian, with -Z as the "straight ahead" axis.
    local nx = -math.sin(gen_yaw) * math.cos(gen_pitch)
    local ny =  math.sin(gen_pitch)
    local nz = -math.cos(gen_yaw) * math.cos(gen_pitch)

    -- New point: one RING_SPACE step along (nx,ny,nz) from the current tail.
    local tail = path[PBUF]
    local new_pt = {
        tail[1] + nx * RING_SPACE,
        tail[2] + ny * RING_SPACE,
        tail[3] + nz * RING_SPACE,
    }

    -- Shift: drop the oldest point (behind camera) and push the new one.
    table.remove(path, 1)
    path[PBUF] = new_pt

    -- Track how many steps have fired so helix phase can compensate (see below).
    ring_offset = ring_offset + 1
end

-- ── Lifecycle ─────────────────────────────────────────────────────────────────

function on_load()
    math.randomseed()
    path_init()
    shader_set("chromatic_ab", "scanlines", "glitch")
      shader_set_uniform("u_glitch_amount", 0.6)
end

function on_osc(addr, ...)
    local args = {...}
    if addr == "/color" then
        cr = args[1] or cr
        cg = args[2] or cg
        cb = args[3] or cb
    elseif addr == "/speed" then
        speed = math.max(0.5, math.min(15.0, args[1] or speed))
    elseif addr == "/left"  then imp_yaw   = imp_yaw   - 0.28
    elseif addr == "/right" then imp_yaw   = imp_yaw   + 0.28
    elseif addr == "/up"    then imp_pitch = imp_pitch + 0.22
    elseif addr == "/down"  then imp_pitch = imp_pitch - 0.22
    end
end

function on_frame(dt)
    t = t + dt

    -- ── Advance camera along the path ────────────────────────────────────────
    -- travel counts how far (in ring-lengths) the camera has moved since the
    -- last path_step.  When it reaches 1.0 we shift the path buffer forward.
    travel = travel + dt * speed
    while travel >= 1.0 do
        path_step()
        travel = travel - 1.0
    end

    -- ── Camera position: interpolate between path[1] and path[2] ─────────────
    local p1, p2 = path[1], path[2]
    local cam_x = p1[1] + (p2[1] - p1[1]) * travel
    local cam_y = p1[2] + (p2[2] - p1[2]) * travel
    local cam_z = p1[3] + (p2[3] - p1[3]) * travel

    -- ── Look target: 7 ring-lengths ahead (interpolated for smooth tracking) ──
    -- Looking further ahead smooths out camera rotation on tight curves —
    -- the camera "sees around the corner" earlier, so turns feel gradual.
    local lt1, lt2 = path[8], path[9]
    local look_x = lt1[1] + (lt2[1] - lt1[1]) * travel
    local look_y = lt1[2] + (lt2[2] - lt1[2]) * travel
    local look_z = lt1[3] + (lt2[3] - lt1[3]) * travel

    -- Wide FOV (69°) emphasises tunnel depth; auto aspect from screen globals.
    perspective_3d(1.2, 0.05, 300.0)
    camera_3d(cam_x, cam_y, cam_z,  look_x, look_y, look_z)

    -- ── Clear + subtle feedback trail ────────────────────────────────────────
    -- clear() fills the FBO with black first, then draw_feedback blends the
    -- previous final frame on top at low alpha — gives a motion-blur trail
    -- without the runaway brightening you'd get without the clear.
    clear(0, 0, 0, 1)
    draw_feedback(0.22)

    -- ── Draw rings back-to-front so near dots overdraw far ones ──────────────
    for ring_i = NUM_RINGS, 1, -1 do
        local idx = ring_i + 1   -- path index: ring 1 = path[2], etc.

        -- Ring centre: fixed world position.  The camera interpolates toward
        -- each ring via 'travel'; the ring itself stays put so it approaches
        -- the viewer naturally.  (Interpolating rings by travel too would make
        -- them track the camera — eliminating the apparent forward motion.)
        local pc = path[idx]
        local rc_x, rc_y, rc_z = pc[1], pc[2], pc[3]

        -- Tangent: central-difference across neighbours for a smooth normal.
        local p_prev = path[math.max(1, idx - 1)]
        local p_next = path[math.min(PBUF, idx + 1)]
        local tx, ty, tz = norm3(
            p_next[1] - p_prev[1],
            p_next[2] - p_prev[2],
            p_next[3] - p_prev[3]
        )

        -- Perpendicular frame: right + up vectors in the ring plane.
        local rx, ry, rz, ux, uy, uz = frame_from_tangent(tx, ty, tz)

        -- Depth cues: both fade and dot size taper toward zero at far end.
        local depth_frac = ring_i / NUM_RINGS           -- 0 = near, 1 = far
        local fade       = 1.0 - depth_frac * 0.88      -- 1.0 → 0.12
        local dot_size   = math.max(1.0, 7.0 * (1.0 - depth_frac))

        -- Near-ring fade: as a ring approaches the camera it reaches extreme
        -- projection angles and its dots scatter off-screen.  When path_step()
        -- fires and drops the ring, the sudden reset creates a visible "pop".
        -- Fix: fade the ring out linearly as its centre gets within half a
        -- ring-length of the camera.  The pop still happens, but the ring is
        -- already invisible by then so there's nothing to see.
        local dx = rc_x - cam_x
        local dy = rc_y - cam_y
        local dz = rc_z - cam_z
        local dist      = math.sqrt(dx*dx + dy*dy + dz*dz)
        local near_fade = math.min(1.0, dist / (RING_SPACE * 0.5))

        -- Skip rings that are essentially invisible — saves draw calls too.
        if near_fade >= 0.01 then
            set_stroke(cr * fade * near_fade, cg * fade * near_fade, cb * fade * near_fade, 1.0)
            set_stroke_weight(dot_size)

            -- Place DOTS dots evenly around the ring.
            -- HELIX adds a per-ring phase offset so consecutive rings are rotated
            -- slightly, making the dots spiral down the tunnel instead of lining
            -- up in visible columns.
            --
            -- Use (ring_i + ring_offset), NOT ring_i alone.  ring_i is the
            -- ring's camera-relative index — it decreases by 1 for every ring
            -- each time path_step() fires (all rings renumber).  Without the
            -- offset every ring snaps by -HELIX radians at each step, producing
            -- a periodic ~10° CCW jerk.  ring_offset increments by +1 per step,
            -- so (ring_i + ring_offset) is constant for each world-space ring
            -- position: helix phase is now stable.
            local phase_offset = (ring_i + ring_offset) * HELIX

            for d = 0, DOTS - 1 do
                local angle = (d / DOTS) * math.pi * 2 + phase_offset
                local ca = math.cos(angle)
                local sa = math.sin(angle)

                -- World position: ring centre + radius * (cos·right + sin·up)
                local wx = rc_x + RING_R * (ca * rx + sa * ux)
                local wy = rc_y + RING_R * (ca * ry + sa * uy)
                local wz = rc_z + RING_R * (ca * rz + sa * uz)

                local sx, sy = project_3d(wx, wy, wz)
                if sx then
                    draw_point(sx, sy)
                end
            end
        end
    end
end
