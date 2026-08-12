-- scenes/reactive_test.lua
-- Driving visuals from a music system: OSC routing, live parameters, and the
-- musical clock.
--
-- Send it something. From SuperCollider or PD:
--
--   /beat  <n>            -- once per beat; tempo is inferred from the timing
--   /kick                 -- fires an envelope
--   /snare                -- fires another
--   /level <0..1> <index> -- a level addressed to one of 8 meters
--   /hue   <0..1>         -- parameter, live-controllable
--   /width <px>           -- parameter, clamped to 2..80
--   /bands <f> x 16       -- 16 values, handed to the shader as a texture
--
-- With nothing connected it still runs: the clock free-runs and the parameters
-- sit at the defaults written below. Press P to see them, 0 to reset them.

-- ── Events ───────────────────────────────────────────────────────────────────
-- One handler per address, arguments named. No if/elseif ladder, and adding
-- another address below never touches what is already here.

on("/kick",  function() env_trigger("kick")  end)
on("/snare", function() env_trigger("snare") end)

on("/level", function(level, index)
    -- The /left_vu shape: a value plus a selector saying which meter it is for.
    persist.meters = persist.meters or {}
    persist.meters[math.floor(index)] = level
end)

on("/bands", function(...)
    -- A whole spectrum in one message, handed straight to the shader.
    persist.bands = { ... }
end)

function on_load()
    beats_per_bar(4)
    persist.meters = persist.meters or {}
end

function on_frame(dt)
    -- ── Parameters ───────────────────────────────────────────────────────────
    -- Declared where they are used. Edit a default here and save: the value
    -- follows the file until OSC sends that address, after which live control
    -- wins and saving no longer disturbs it.
    local hue   = param("hue",   0.55, 0, 1)
    local width = param("width", 24,   2, 80)
    local trail = param("trail", 0.88, 0, 0.99)

    draw_feedback(trail)
    set_blend("add")

    local W, H = screen_width, screen_height
    local cx, cy = W / 2, H / 2

    -- ── Musical time ─────────────────────────────────────────────────────────
    -- bar_phase() sweeps 0..1 across the bar and keeps advancing between /beat
    -- messages, so this ring turns smoothly rather than jumping once a beat.
    -- With no clock running it free-runs at the last known tempo.
    local sweep = bar_phase() * math.pi * 2
    local kick  = env("kick",  0.12)
    local snare = env("snare", 0.25)

    -- Ring of markers, one per beat in the bar, the current one enlarged.
    local beats = beats_per_bar()
    for i = 0, beats - 1 do
        local a  = (i / beats) * math.pi * 2 - math.pi / 2
        local on_this = (beat_count() % beats) == i
        local r  = on_this and (10 + kick * 14) or 5
        local p  = vec(cx, cy) + vec.from_angle(a) * (H * 0.32)
        set_color_hsv(hue + i * 0.02, 0.8, on_this and 1.0 or 0.45)
        draw_circle(p.x, p.y, r)
    end

    -- Sweep hand, driven by the continuous bar phase.
    set_stroke_hsv(hue + 0.1, 0.7, 0.9, 0.8)
    set_stroke_weight(2)
    local hand = vec(cx, cy) + vec.from_angle(sweep - math.pi / 2) * (H * 0.28)
    draw_line(cx, cy, hand.x, hand.y)

    -- Centre pulse on the kick.
    set_color_hsv(hue, 0.4, 1.0, 0.6 * kick)
    draw_circle(cx, cy, 20 + kick * 90 + snare * 40)

    -- ── Meters ───────────────────────────────────────────────────────────────
    -- Whatever /level has delivered, one bar per index. They persist across a
    -- reload, so editing this drawing code does not blank the display.
    for i = 1, 8 do
        local v = persist.meters[i] or 0
        set_color_hsv(hue + v * 0.25, 0.9, 0.4 + v * 0.6)
        draw_rect(20 + (i - 1) * (width + 6), H - 20 - v * 160, width, v * 160)
    end

    -- ── Status ───────────────────────────────────────────────────────────────
    set_blend("alpha")
    set_color(0.5, 0.7, 0.65, 1)
    draw_text(16, 16, string.format(
        "bpm %.1f  bar %.2f  beat %d  %s",
        bpm(), bar_phase(), beat_count(),
        beat_active() and "locked" or "free-running"), 2)
end
