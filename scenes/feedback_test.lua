-- scenes/feedback_test.lua
-- Phase 5 acceptance test: persistent feedback trails.
--
-- No clear() is called — instead draw_feedback() blends the previous frame
-- at 97% alpha over a black-cleared FBO.  Each new frame a random circle
-- is drawn on top, leaving a trail that slowly fades to black.
--
-- Alpha decay rate: at 60fps, 0.97^N reaches 5% brightness at N≈98 frames
-- (~1.6 seconds), giving a clearly visible long tail.
--
-- You should see:
--   - White circles appearing at random positions
--   - Trails lasting ~1.5 seconds before fading to black
--   - No flickering or tearing — the feedback texture persists cleanly

math.randomseed(42)

function on_frame(dt)
    -- Blend the previous frame's image at 97% alpha over the cleared FBO.
    -- (begin_frame() already cleared the FBO to black, so areas with no
    -- feedback content will simply remain black.)
    draw_feedback(0.99, 1.0, 0.0)

    -- Draw a new circle each frame at a random position.
    set_color(1, 1, 1, 1)
    draw_circle(
        math.random() * screen_width,
        math.random() * screen_height,
        8
    )
end
