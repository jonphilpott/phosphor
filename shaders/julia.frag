#version 140

// ── julia.frag ────────────────────────────────────────────────────────────────
// Generates a Julia set — ignores u_texture entirely.
//
// A Julia set uses a fixed complex constant c; each pixel's starting z is its
// screen position.  Different values of c give wildly different shapes.
// Animating c (e.g. moving it in a circle) morphs the set continuously.
//
// Use as a full-frame post-process:
//   shader_set("julia")
//
// Or render into a canvas for compositing:
//   c:begin() clear(0,0,0,1) c:finish("julia")
//   c:draw(x, y, w, h, angle)
//
// Control via shader_set_uniform():
//   u_julia_cx    — real part of c      (default −0.7)
//   u_julia_cy    — imaginary part of c (default  0.27)
//   u_zoom        — zoom level          (default 1.0)
//   u_max_iter    — iteration cap       (default 128)
//   u_color_shift — palette phase       (default 0.0)
//   u_color_speed — auto cycle speed    (default 0.05)
//   u_animate     — orbit radius for c  (default 0.0; set > 0 to auto-morph)
//
// Interesting c values to explore:
//   (-0.70,  0.27)  — Siegel disk (default, symmetric)
//   (-0.75,  0.11)  — Douady rabbit
//   (-0.12,  0.74)  — Dendrite (near Misiurewicz point)
//   ( 0.28,  0.01)  — Fat fractal
//
// Animating c along a circle:
//   -- in on_frame(dt):
//   shader_set_uniform("u_animate", 0.4)   -- orbit radius = 0.4
//   -- u_time drives the orbit automatically

uniform sampler2D u_texture;    // required by pipeline — not sampled
uniform vec2      u_resolution;
uniform float     u_time;

uniform float u_julia_cx;
uniform float u_julia_cy;
uniform float u_zoom;
uniform float u_max_iter;
uniform float u_color_shift;
uniform float u_color_speed;
uniform float u_animate;        // if > 0, orbits c around (u_julia_cx, u_julia_cy)

in  vec2 v_uv;
out vec4 frag_color;

vec3 palette(float t) {
    // Warmer palette than mandelbrot.frag — more orange/red for differentiation.
    vec3 a = vec3(0.5, 0.5, 0.5);
    vec3 b = vec3(0.5, 0.5, 0.5);
    vec3 c = vec3(1.0, 0.7, 0.4);
    vec3 d = vec3(0.00, 0.15, 0.20);
    return a + b * cos(6.28318 * (c * t + d));
}

void main() {
    // ── Step 1: Map UV to complex plane ──────────────────────────────────────
    float aspect = u_resolution.x / u_resolution.y;
    vec2 uv = (v_uv * 2.0 - 1.0) * vec2(aspect, 1.0);

    float zoom = max(u_zoom, 0.0001);

    // For a Julia set, z starts at the screen coordinate (not at 0 like Mandelbrot).
    vec2 z = uv / zoom;

    // ── Step 2: Determine the Julia constant c ────────────────────────────────
    // If u_animate > 0, orbit c around (u_julia_cx, u_julia_cy) at radius u_animate.
    // This morphs the fractal shape over time — a common live performance trick.
    // Default c when uniforms are unset (all zero): use (-0.7, 0.27) as fallback.
    float base_cx = (u_julia_cx == 0.0 && u_julia_cy == 0.0) ? -0.7 : u_julia_cx;
    float base_cy = (u_julia_cx == 0.0 && u_julia_cy == 0.0) ?  0.27 : u_julia_cy;
    vec2 jc = vec2(
        base_cx + cos(u_time * 0.3) * u_animate,
        base_cy + sin(u_time * 0.3) * u_animate
    );

    // ── Step 3: Iterate  z ← z² + c ─────────────────────────────────────────
    int max_i = max(int(u_max_iter), 32);
    int iter  = max_i;

    for (int i = 0; i < 512; i++) {
        if (i >= max_i) break;
        z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + jc;
        if (dot(z, z) > 256.0) { iter = i; break; }
    }

    // ── Step 4: Colour ────────────────────────────────────────────────────────
    if (iter == max_i) {
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float mu = float(iter) + 1.0 - log2(log2(length(z)));
    float t  = mu / float(max_i);

    vec3 col = palette(t * 3.0 + u_color_shift + u_time * u_color_speed);
    frag_color = vec4(col, 1.0);
}
