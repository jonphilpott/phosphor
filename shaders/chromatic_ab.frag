#version 330 core

// ── chromatic_ab.frag ─────────────────────────────────────────────────────────
// Chromatic aberration: samples the R, G and B channels at slightly offset UVs.
//
// Real lenses focus different wavelengths at different points — red bends less
// than blue.  We simulate this by shifting the UV coordinates of each channel
// by a small amount in opposite directions.
//
// u_chrom_amount: half the total channel separation in UV units (default 0.002).
// At 1920px wide, 0.002 = ~4 pixels separation.  Use 0.005–0.01 for drama.
//
// The offset direction is radial from the UV centre (0.5, 0.5) so the effect
// is stronger at the corners, just like a real lens.

uniform sampler2D u_texture;
uniform vec2      u_resolution;
uniform float     u_chrom_amount;  // set via shader_set_uniform("chrom_amount", 0.003)

in  vec2 v_uv;
out vec4 frag_color;

void main() {
    // Default offset if u_chrom_amount is not set (uniform defaults to 0.0 in GLSL).
    float amount = (u_chrom_amount > 0.0) ? u_chrom_amount : 0.002;

    // Direction vector from the centre of the screen toward this fragment.
    // Normalising gives a unit direction; multiplying by amount gives a small offset.
    vec2 dir = normalize(v_uv - vec2(0.5));
    vec2 offset = dir * amount;

    // Sample each colour channel with a slightly different UV.
    // Red is pushed outward, blue is pushed inward — matches typical lens behaviour.
    float r = texture(u_texture, v_uv + offset).r;
    float g = texture(u_texture, v_uv         ).g;
    float b = texture(u_texture, v_uv - offset).b;
    float a = texture(u_texture, v_uv         ).a;

    frag_color = vec4(r, g, b, a);
}
