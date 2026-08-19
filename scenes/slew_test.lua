-- scenes/slew_test.lua
-- slew() — variables that glide to a new value instead of jumping to it.
--
-- Three traces scroll right to left:
--
--   dim      the raw target, a square wave flipping every 2 seconds
--   bright   a slew, advanced by the engine — the scene never touches dt
--   dotted   the same curve built by hand with smooth_hl(), for comparison
--
-- The bright line and the dots should sit on top of each other. That is the
-- test: it proves the C++ slew and the Lua easing helper agree, and because
-- both are exponential in dt rather than per-frame, it holds however the frame
-- rate wanders.
--
-- Things worth trying while it runs:
--
--   [ and ]   change the time scale — the glide stretches with everything else
--   space     pause — the glide freezes where it stands, then resumes
--   0         reset — puts SLEW_TIME's value back to the file default
--   save this file mid-glide — the bright line keeps travelling, because the
--             slew has been set() and so no longer follows the file

local PERIOD    = 2.0    -- seconds the square wave holds each level
local SLEW_TIME = 1.0    -- seconds to cover 99% of the distance
local HISTORY   = 240    -- samples kept for the scrolling traces

-- smooth_hl() is parameterised by half-life, slew() by time-to-99%, so one has
-- to be converted into the other before they can be compared. Both are of the
-- form (fraction remaining)^(dt/time), so they describe the same curve when
--
--     0.5 ^ (dt / half_life)  ==  0.01 ^ (dt / slew_time)
--
-- which rearranges to the line below. Computed rather than typed as a literal:
-- rounding the constant to a few decimals is enough to push the two curves
-- visibly apart over a couple of seconds, which would look like a bug here.
local HALF_LIFE = SLEW_TIME * math.log(0.5) / math.log(0.01)

function on_load()
    persist.trace = {}
    persist.hand  = 0        -- the hand-rolled comparison value
    persist.last  = nil      -- last target, so we only set() on a change
end

function on_frame(dt)
    -- ── The slew ─────────────────────────────────────────────────────────────
    -- Declared right where it is used and safe to call every frame, exactly
    -- like param(). The handle is the same object each time.
    local level = slew("level", 0.0, SLEW_TIME)

    -- A square wave: 0, then 1, then 0, flipping every PERIOD seconds.
    local target = (math.floor(elapsed() / PERIOD) % 2 == 0) and 0.0 or 1.0

    -- Only retarget when the square wave actually flips. Calling set() every
    -- frame with the same value would work identically — the target simply
    -- would not change — but this makes the intent obvious.
    if target ~= persist.last then
        level.set(target)
        persist.last = target
    end

    -- ── The hand-rolled equivalent ───────────────────────────────────────────
    -- What you would write without slew(): somewhere to park the value, a call
    -- every frame, and dt threaded through to it.
    persist.hand = smooth_hl(persist.hand, target, HALF_LIFE, dt)

    -- ── Record and scroll ────────────────────────────────────────────────────
    table.insert(persist.trace, { target, level.get(), persist.hand })
    while #persist.trace > HISTORY do table.remove(persist.trace, 1) end

    local W, H = screen_width, screen_height
    local top, bot = H * 0.25, H * 0.75
    local step = W / HISTORY

    -- Baselines for 0 and 1, so the overshoot-free approach is visible.
    set_stroke(0.15, 0.18, 0.2, 1)
    set_stroke_weight(1)
    draw_line(0, top, W, top)
    draw_line(0, bot, W, bot)

    -- The traces. y maps 0 -> bot and 1 -> top.
    local function y_of(v) return bot + (top - bot) * v end

    for i = 2, #persist.trace do
        local a, b = persist.trace[i - 1], persist.trace[i]
        local x0, x1 = (i - 2) * step, (i - 1) * step

        -- Raw target, dim: the steps we are smoothing away.
        set_stroke(0.3, 0.3, 0.35, 1)
        set_stroke_weight(1)
        draw_line(x0, y_of(a[1]), x1, y_of(b[1]))

        -- The slew, bright.
        set_stroke_hsv(0.5, 0.7, 1.0, 1)
        set_stroke_weight(2)
        draw_line(x0, y_of(a[2]), x1, y_of(b[2]))
    end

    -- The hand-rolled curve as dots, so it reads as an overlay rather than a
    -- second line. If it drifts off the bright trace, the maths disagrees.
    set_color(1.0, 0.4, 0.2, 0.9)
    for i = 1, #persist.trace, 6 do
        draw_circle((i - 1) * step, y_of(persist.trace[i][3]), 2)
    end

    -- ── Readout ──────────────────────────────────────────────────────────────
    set_color(0.5, 0.7, 0.65, 1)
    draw_text(16, 16, string.format(
        "target %.3f   slew %.3f   smooth_hl %.3f   delta %.5f",
        target, level.get(), persist.hand,
        math.abs(level.get() - persist.hand)), 2)
    draw_text(16, 34, string.format(
        "slew_time %.2fs   dt %.4f   %s",
        SLEW_TIME, dt, tostring(level)), 2)
end
