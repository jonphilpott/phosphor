-- scenes/text_test.lua
-- Exercises the bitmap text renderer across all features:
--   1. All printable ASCII characters in one block (font glyph check)
--   2. Scale 1, 2, 3, 4 comparison
--   3. Horizontal centering via text_width()
--   4. Multi-line text
--   5. Colour changes
--   6. Text through the transform stack (translate + rotate)

local t = 0.0

function on_load()
    shader_set("scanlines")
end

function on_frame(dt)
    t = t + dt
    clear(0.05, 0.05, 0.05, 1)

    local cx = screen_width  / 2
    local cy = screen_height / 2

    -- ── Section 1: full printable ASCII glyph dump ────────────────────────────
    -- Two rows of 48 characters each cover the full 0x20–0x7E range.
    -- Rendered at scale 1 so every glyph is clearly visible at native size.
    set_color(0.6, 0.6, 0.6, 1)
    local ascii_line1 = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNO"
    local ascii_line2 = "PQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
    local dump_x = math.floor((screen_width - text_width(ascii_line1, 1)) / 2)
    draw_text(dump_x, 16, ascii_line1, 1)
    draw_text(dump_x, 26, ascii_line2, 1)

    -- ── Section 2: scale ladder ───────────────────────────────────────────────
    -- Same word at 1×, 2×, 3×, 4× so glyph quality at each scale is obvious.
    local scales  = { 1, 2, 3, 4 }
    local scale_y = 52
    for _, s in ipairs(scales) do
        local label = "Aa1!@" .. tostring(s) .. "x"
        local w     = text_width(label, s)
        set_color(0.3 + s * 0.15, 0.8, 1.0 - s * 0.1, 1)
        draw_text(math.floor(cx - w / 2), scale_y, label, s)
        scale_y = scale_y + 8 * s + 4
    end

    -- ── Section 3: centred title with pulsing colour ──────────────────────────
    local title = "PHOSPHOR"
    local tw    = text_width(title, 4)
    local pulse = 0.6 + 0.4 * math.sin(t * 2.0)
    set_color(pulse, 1.0 - pulse * 0.3, 0.2, 1)
    draw_text(math.floor(cx - tw / 2), math.floor(cy - 16), title, 4)

    -- ── Section 4: multi-line block ───────────────────────────────────────────
    set_color(0.4, 1.0, 0.4, 1)
    local ml = "line one\nline two\nline three"
    draw_text(20, math.floor(cy + 30), ml, 2)

    -- ── Section 5: right-aligned text ────────────────────────────────────────
    -- text_width lets you right- or centre-align without a layout engine.
    set_color(1.0, 0.8, 0.2, 1)
    local lines = { "right", "aligned", "text" }
    local ry    = math.floor(cy + 30)
    for _, line in ipairs(lines) do
        local lw = text_width(line, 2)
        draw_text(screen_width - 20 - lw, ry, line, 2)
        ry = ry + 18
    end

    -- ── Section 6: text through the transform stack ───────────────────────────
    -- Shows that push/translate/rotate affects text just like rectangles.
    local spin_speed = 0.4
    set_color(1.0, 0.4, 0.8, 1)
    push()
        translate(cx, screen_height - 40)
        rotate(math.sin(t * spin_speed) * 0.25)
        local spin_str = "< transform stack >"
        local sw = text_width(spin_str, 1)
        draw_text(math.floor(-sw / 2), -4, spin_str, 1)
    pop()

    -- ── Section 7: live readout ───────────────────────────────────────────────
    -- Practical use: drawing a debug string with a formatted number each frame.
    set_color(0.5, 0.5, 0.5, 1)
    draw_text(4, screen_height - 12,
        string.format("t=%.1f  %dx%d", t, screen_width, screen_height), 1)
end
