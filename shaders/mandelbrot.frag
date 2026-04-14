#version 140

// ── mandelbrot.frag ───────────────────────────────────────────────────────────
// Generates a Mandelbrot set — ignores u_texture entirely.
//
// Use as a full-frame post-process:
//   shader_set("mandelbrot")
//
// Or render into a canvas for position/scale/rotation control:
//   c:begin() clear(0,0,0,1) c:finish("mandelbrot")
//   c:draw(x, y, w, h, angle)
//
// Control via shader_set_uniform():
//   u_center_x    — real axis pan          (default 0.0; set -0.5 for classic view)
//   u_center_y    — imaginary axis pan     (default 0.0)
//   u_zoom        — zoom level             (default 1.0, higher = more zoomed in)
//   u_max_iter    — iteration cap 32–512   (default 128, higher = more detail)
//   u_color_shift — palette phase offset   (default 0.0)
//   u_color_speed — automatic color cycle speed (default 0.05)
//
// Example from Lua:
//   function on_load()
//       shader_set("mandelbrot")
//       shader_set_uniform("u_center_x",   -0.5)
//       shader_set_uniform("u_zoom",        1.2)
//       shader_set_uniform("u_color_speed", 0.1)
//   end

uniform sampler2D u_texture;    // required by pipeline — not sampled
uniform vec2      u_resolution;
uniform float     u_time;

uniform float u_center_x;
uniform float u_center_y;
uniform float u_zoom;
uniform float u_max_iter;
uniform float u_color_shift;
uniform float u_color_speed;

in  vec2 v_uv;
out vec4 frag_color;

// ── Cosine colour palette (Inigo Quilez) ─────────────────────────────────────
// Maps a scalar t → RGB using offset cosine waves.
// The d vector shifts the phase of each channel, producing a distinct hue cycle.
vec3 palette(float t) {
    vec3 a = vec3(0.5, 0.5, 0.5);
    vec3 b = vec3(0.5, 0.5, 0.5);
    vec3 c = vec3(1.0, 1.0, 1.0);
    vec3 d = vec3(0.00, 0.33, 0.67);   // phase offset per channel → blue-yellow-orange cycle
    return a + b * cos(6.28318 * (c * t + d));
}

void main() {
    // ── Step 1: Map UV to complex plane ──────────────────────────────────────
    // v_uv is [0,1]×[0,1]; remap to [-1,1]×[-1,1] and correct for aspect ratio
    // so the set isn't stretched on non-square viewports.
    float aspect = u_resolution.x / u_resolution.y;
    vec2 uv = (v_uv * 2.0 - 1.0) * vec2(aspect, 1.0);

    // ── Step 2: Apply zoom and pan ────────────────────────────────────────────
    // Guard against an unset u_zoom uniform (defaults to 0.0).
    float zoom = max(u_zoom, 0.0001);
    vec2 c = uv / zoom + vec2(u_center_x, u_center_y);

    // ── Step 3: Iterate  z ← z² + c  starting from z = 0 ────────────────────
    // Complex multiplication: (a+bi)² = (a²−b²) + (2ab)i
    // We use a compile-time bound of 512 with an early-out on u_max_iter so
    // the GPU doesn't need a variable loop limit (some drivers dislike those).
    int max_i = max(int(u_max_iter), 32);
    vec2 z    = vec2(0.0);
    int  iter = max_i;   // assume interior until proven otherwise

    for (int i = 0; i < 512; i++) {
        if (i >= max_i) break;
        z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + c;
        // Escape radius 16 (test |z|²>256): larger radius gives smoother colouring.
        if (dot(z, z) > 256.0) { iter = i; break; }
    }

    // ── Step 4: Colour ────────────────────────────────────────────────────────
    if (iter == max_i) {
        // Interior of the set → solid black.
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Smooth (continuous) iteration count — eliminates colour banding that
    // occurs when integer iter values jump abruptly at the escape boundary.
    // The log2(log2()) term compensates for the non-integer escape point.
    float mu = float(iter) + 1.0 - log2(log2(length(z)));
    float t  = mu / float(max_i);

    // Map to palette with manual shift and optional time-driven animation.
    vec3 col = palette(t * 4.0 + u_color_shift + u_time * u_color_speed);
    frag_color = vec4(col, 1.0);
}
