-- scenes/easing.lua
-- Utility functions for smooth value transitions and range mapping.
--
-- Load in any scene with:
--   local easing = dofile("scenes/easing.lua")
--
-- All functions are pure (no side effects, no global state) so the module is
-- safe to load multiple times or share across scenes.

local M = {}

-- ── Linear interpolation ──────────────────────────────────────────────────────
-- Returns the value that is fraction t of the way from a to b.
-- t=0 → a,  t=1 → b,  t=0.5 → midpoint.
-- t is NOT clamped — you can extrapolate beyond [a,b] if you pass t outside [0,1].
function M.lerp(a, b, t)
    return a + (b - a) * t
end

-- ── Exponential smoothing (rate form) ────────────────────────────────────────
-- Moves 'current' toward 'target' by a fraction that depends on elapsed time.
-- 'rate' controls how fast: higher = snappier.  Rule of thumb:
--   rate = 1  → roughly 63% of the gap closed per second
--   rate = 5  → roughly 99% closed per second
--   rate = 10 → very snappy, nearly instant
--
-- Why not just lerp by a fixed amount each frame?  Because lerp(x, target, 0.1)
-- every frame is frame-rate dependent — at 30fps it moves twice as far as at 60fps.
-- This formula uses dt to make the speed frame-rate independent:
--   new = target + (current - target) * e^(-rate * dt)
function M.exp(current, target, rate, dt)
    -- math.exp(-rate * dt) approaches 0 as dt or rate increase, so the gap
    -- shrinks proportionally each frame regardless of frame rate.
    return target + (current - target) * math.exp(-rate * dt)
end

-- ── Exponential smoothing (half-life form) ────────────────────────────────────
-- Same idea as M.exp but parameterised by half_life: the time in seconds for
-- the gap to halve.  More intuitive when you're thinking in musical time:
--   half_life = 0.1  → snappy (gap halves every 100 ms)
--   half_life = 0.5  → moderate
--   half_life = 2.0  → very lazy drift
function M.exp_hl(current, target, half_life, dt)
    if half_life <= 0 then return target end
    -- 0.5^(dt/half_life): at dt=half_life the factor is exactly 0.5.
    return target + (current - target) * (0.5 ^ (dt / half_life))
end

-- ── Range mapping ─────────────────────────────────────────────────────────────
-- Remaps x from [in_lo, in_hi] to [out_lo, out_hi].
-- Does NOT clamp — use M.clamp afterwards if you need hard limits.
-- Example: map an OSC value 0..1 to screen X 100..1820:
--   local sx = easing.map(osc_val, 0, 1, 100, 1820)
function M.map(x, in_lo, in_hi, out_lo, out_hi)
    -- Normalise x to [0,1] within the input range, then scale to output range.
    return out_lo + (x - in_lo) * (out_hi - out_lo) / (in_hi - in_lo)
end

-- ── Clamp ─────────────────────────────────────────────────────────────────────
-- Constrains x to [lo, hi].  math.max/min idiom — just named for readability.
function M.clamp(x, lo, hi)
    return math.max(lo, math.min(hi, x))
end

-- ── Smoothstep ────────────────────────────────────────────────────────────────
-- S-curve: maps t in [0,1] to a smooth [0,1] with zero derivative at both ends.
-- Useful for fade-ins/outs that don't feel abrupt at the start or end.
-- The formula 3t²-2t³ is the classic cubic Hermite interpolant.
function M.smoothstep(t)
    t = M.clamp(t, 0, 1)
    return t * t * (3 - 2 * t)
end

-- ── Pulse ─────────────────────────────────────────────────────────────────────
-- Returns 1 when t (in seconds) is within 'width' seconds of a beat at 'bpm',
-- fading out smoothly between beats.  Good for flash/strobe effects driven by
-- u_beat or a local time accumulator.
--   local flash = easing.pulse(t, 120, 0.05)   -- 120bpm, 50ms flash
function M.pulse(t, bpm, width)
    -- Convert time to beat position within one bar, then measure distance to
    -- the nearest beat boundary.
    local beat_period = 60.0 / bpm
    local phase       = (t % beat_period) / beat_period  -- 0..1 within one beat
    -- Distance from the downbeat (0) wrapping at 1.
    local dist        = math.min(phase, 1 - phase) * beat_period
    return M.smoothstep(1 - dist / width)
end

return M
