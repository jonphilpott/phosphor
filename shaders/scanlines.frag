#version 330 core

// ── scanlines.frag ────────────────────────────────────────────────────────────
// Dims every other horizontal row to simulate a CRT scanline pattern.
//
// gl_FragCoord.y is the fragment's Y position in window coordinates (pixels).
// mod(y, 2.0) gives 0..2 for alternating rows.
// Rows where mod < 1 are dimmed to 80%, giving a subtle interleave effect.
//
// The dimming factor (0.8) and period (2.0 rows) are intentionally hardcoded —
// this is an aesthetic shader, not a general utility.

uniform sampler2D u_texture;
uniform vec2      u_resolution;   // not used here, but available for subclasses

in  vec2 v_uv;
out vec4 frag_color;

void main() {
    vec4 color = texture(u_texture, v_uv);

    // gl_FragCoord.y counts from the bottom of the viewport in OpenGL.
    // We group rows into pairs (floor + mod), so each visible "scanline" is
    // 2 FBO pixels tall — at Retina 2× that maps to 1 logical pixel, giving
    // a clean CRT-style pattern at any resolution.
    // Dark rows are at 30% brightness (70% dimming) for a clearly visible effect.
    float scanline = mod(floor(gl_FragCoord.y / 2.0), 2.0) < 1.0 ? 0.3 : 1.0;

    frag_color = vec4(color.rgb * scanline, color.a);
}
