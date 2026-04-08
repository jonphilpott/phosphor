#version 330 core

// ── glitch.frag ───────────────────────────────────────────────────────────────
// Digital glitch / signal corruption effect.
//
// Simulates the kind of artefacts you get from a damaged video signal or
// corrupted frame buffer:
//   - Scanline displacement  : thin horizontal bands shifted left/right
//   - Block displacement     : larger rectangular chunks shifted
//   - RGB channel split      : R and B sample from different X positions in
//                              glitched areas (like chromatic_ab but asymmetric)
//   - Horizontal tear        : a strip of the image relocated to a random Y
//   - Signal dropout         : occasional bands collapse to black
//
// u_glitch_amount : overall intensity, 0.0–1.0 (default 0.35 if not set).
//   At 0.0 the effect vanishes; at 1.0 the signal is heavily corrupted.
//   Control from Lua: shader_set_uniform("u_glitch_amount", 0.6)
//
// u_time is used to step the glitch in discrete time slots — this is the key
// to making it feel "digital" rather than smooth/organic.  floor(u_time * N)
// gives N discrete states per second; each state gets its own random seed so
// every slot looks different.

uniform sampler2D u_texture;
uniform vec2      u_resolution;
uniform float     u_time;
uniform float     u_beat;
uniform float     u_glitch_amount;

in  vec2 v_uv;
out vec4 frag_color;

// ── Pseudo-random hash ────────────────────────────────────────────────────────
// Maps a 2D float seed to a scalar in [0, 1).
// Technique: dot the seed with two large irrational-ish constants, take sin,
// then keep only the fractional part.  The result looks random but is fully
// deterministic — same seed always gives the same output.  Not cryptographic,
// but more than adequate for visual noise.
float rand(vec2 seed) {
    return fract(sin(dot(seed, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    // If the caller hasn't set u_glitch_amount it defaults to 0.0 in GLSL.
    // Fall back to a visible default so the shader works with no setup.
    float amount = (u_glitch_amount > 0.001) ? u_glitch_amount : 0.35;

    vec2 uv = v_uv;

    // ── Step 1: Quantise time into discrete "glitch frames" ───────────────────
    // floor() snaps continuous time to integer steps.  Each step has a fixed
    // random state for the whole duration — that's what gives the stepped,
    // digital feel rather than smooth animation.
    //   t_fast: 12 slots/sec — rapid scanline flicker
    //   t_slow:  3 slots/sec — slower block shifts
    //   t_tear:  6 slots/sec — tear position changes
    float t_fast = floor(u_time * 12.0);
    float t_slow = floor(u_time *  3.0);
    float t_tear = floor(u_time *  6.0);

    // ── Step 2: Scanline displacement ─────────────────────────────────────────
    // Divide the screen into horizontal bands (~3px each) using integer division
    // of the pixel Y coordinate.  Each band gets an independent random number.
    // Only bands whose random value exceeds a threshold are displaced — the
    // threshold rises as amount falls, so fewer lines glitch at lower intensity.
    float scan_y  = floor(uv.y * u_resolution.y / 3.0);
    float scan_r  = rand(vec2(scan_y * 0.013, t_fast));
    float scan_on = step(1.0 - 0.10 * amount, scan_r);   // ~10% of bands at full
    float scan_dx = (rand(vec2(scan_y, t_fast + 0.5)) - 0.5) * 0.15 * amount;
    uv.x = fract(uv.x + scan_on * scan_dx);              // fract() wraps at edges

    // ── Step 3: Block displacement ────────────────────────────────────────────
    // Same idea as scanlines but with ~24px blocks and slower time stepping.
    // Independently layered on top of the scanline pass.
    float blk_y  = floor(uv.y * u_resolution.y / 24.0);
    float blk_r  = rand(vec2(blk_y * 0.07, t_slow));
    float blk_on = step(1.0 - 0.06 * amount, blk_r);
    float blk_dx = (rand(vec2(blk_y + 99.0, t_slow)) - 0.5) * 0.22 * amount;
    uv.x = fract(uv.x + blk_on * blk_dx);

    // ── Step 4: RGB channel split in corrupted areas ──────────────────────────
    // In any region that was displaced, shift R rightward and B leftward.
    // This is the same principle as chromatic_ab but driven by the glitch mask
    // rather than being uniform — corruption and fringing appear together.
    float split = (scan_on * 0.006 + blk_on * 0.014) * amount;
    float r = texture(u_texture, vec2(uv.x + split, uv.y)).r;
    float g = texture(u_texture, uv).g;
    float b = texture(u_texture, vec2(uv.x - split, uv.y)).b;

    // ── Step 5: Horizontal tear ───────────────────────────────────────────────
    // Pick a random Y position and height each tear slot.  Any fragment inside
    // that strip samples from a completely different UV — simulating a tape
    // dropout that relocates an entire horizontal section of the image.
    float tear_y  = rand(vec2(t_tear, 3.71));            // random strip Y 0–1
    float tear_h  = rand(vec2(t_tear, 8.23)) * 0.045 * amount;
    // step(a, x) = 1 if x >= a, else 0.  ANDing two steps carves out the strip.
    float in_tear = step(tear_y, uv.y) * step(uv.y, tear_y + tear_h);
    float tear_dx = (rand(vec2(t_tear, 1.11)) - 0.5) * 0.45 * amount;
    float tear_dy = (rand(vec2(t_tear, 5.55)) - 0.5) * 0.12 * amount;
    vec2  tear_uv = vec2(fract(uv.x + tear_dx), fract(uv.y + tear_dy));
    vec4  torn    = texture(u_texture, tear_uv);

    // ── Step 6: Signal dropout ────────────────────────────────────────────────
    // A very small fraction of ~8px bands randomly collapse to black — like the
    // signal dying for a single scanline group.  Very fast time slot so it
    // appears and disappears quickly, giving a "sparkling" corruption.
    float drop_y = floor(uv.y * u_resolution.y / 8.0);
    float drop_r = rand(vec2(drop_y * 0.031, floor(u_time * 24.0)));
    float dropout = step(1.0 - 0.04 * amount, drop_r);

    // ── Composite ─────────────────────────────────────────────────────────────
    vec4 col = vec4(r, g, b, 1.0);
    col = mix(col, torn, in_tear);                        // tear overwrites strip
    col = mix(col, vec4(0.0, 0.0, 0.0, 1.0), dropout);   // dropout → black

    frag_color = col;
}
