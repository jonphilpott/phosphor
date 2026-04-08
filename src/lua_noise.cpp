// lua_noise.cpp
// Perlin noise (Ken Perlin's "Improved Noise", SIGGRAPH 2002) exposed to Lua.
//
// Reference: https://mrl.nyu.edu/~perlin/noise/
// The key improvement over the original 1985 noise is the "fade" polynomial:
//   6t^5 - 15t^4 + 10t^3   (has zero first AND second derivative at t=0 and t=1)
// vs the original 3t^2 - 2t^3 which had a non-zero second derivative — causing
// visible "creases" in the noise field at integer coordinates.

#include "lua_noise.h"

#include <cmath>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ── Perlin permutation table ──────────────────────────────────────────────────
//
// This is the canonical 256-entry permutation from Perlin's 2002 paper.
// Doubled to 512 entries so we can index p[x + p[y]] without a modulo.
// The values are a shuffled 0..255 — no repetitions in the first half.

static const int perm[512] = {
    151,160,137, 91, 90, 15,131, 13,201, 95, 96, 53,194,233,  7,225,
    140, 36,103, 30, 69,142,  8, 99, 37,240, 21, 10, 23,190,  6,148,
    247,120,234, 75,  0, 26,197, 62, 94,252,219,203,117, 35, 11, 32,
     57,177, 33, 88,237,149, 56, 87,174, 20,125,136,171,168, 68,175,
     74,165, 71,134,139, 48, 27,166, 77,146,158,231, 83,111,229,122,
     60,211,133,230,220,105, 92, 41, 55, 46,245, 40,244,102,143, 54,
     65, 25, 63,161,  1,216, 80, 73,209, 76,132,187,208, 89, 18,169,
    200,196,135,130,116,188,159, 86,164,100,109,198,173,186,  3, 64,
     52,217,226,250,124,123,  5,202, 38,147,118,126,255, 82, 85,212,
    207,206, 59,227, 47, 16, 58, 17,182,189, 28, 42,223,183,170,213,
    119,248,152,  2, 44,154,163, 70,221,153,101,155,167, 43,172,  9,
    129, 22, 39,253, 19, 98,108,110, 79,113,224,232,178,185,112,104,
    218,246, 97,228,251, 34,242,193,238,210,144, 12,191,179,162,241,
     81, 51,145,235,249, 14,239,107, 49,192,214, 31,181,199,106,157,
    184, 84,204,176,115,121, 50, 45,127,  4,150,254,138,236,205, 93,
    222,114, 67, 29, 24, 72,243,141,128,195, 78, 66,215, 61,156,180,
    // Second half — identical copy so we can index p[x + p[y]] safely.
    151,160,137, 91, 90, 15,131, 13,201, 95, 96, 53,194,233,  7,225,
    140, 36,103, 30, 69,142,  8, 99, 37,240, 21, 10, 23,190,  6,148,
    247,120,234, 75,  0, 26,197, 62, 94,252,219,203,117, 35, 11, 32,
     57,177, 33, 88,237,149, 56, 87,174, 20,125,136,171,168, 68,175,
     74,165, 71,134,139, 48, 27,166, 77,146,158,231, 83,111,229,122,
     60,211,133,230,220,105, 92, 41, 55, 46,245, 40,244,102,143, 54,
     65, 25, 63,161,  1,216, 80, 73,209, 76,132,187,208, 89, 18,169,
    200,196,135,130,116,188,159, 86,164,100,109,198,173,186,  3, 64,
     52,217,226,250,124,123,  5,202, 38,147,118,126,255, 82, 85,212,
    207,206, 59,227, 47, 16, 58, 17,182,189, 28, 42,223,183,170,213,
    119,248,152,  2, 44,154,163, 70,221,153,101,155,167, 43,172,  9,
    129, 22, 39,253, 19, 98,108,110, 79,113,224,232,178,185,112,104,
    218,246, 97,228,251, 34,242,193,238,210,144, 12,191,179,162,241,
     81, 51,145,235,249, 14,239,107, 49,192,214, 31,181,199,106,157,
    184, 84,204,176,115,121, 50, 45,127,  4,150,254,138,236,205, 93,
    222,114, 67, 29, 24, 72,243,141,128,195, 78, 66,215, 61,156,180
};

// ── Math helpers ──────────────────────────────────────────────────────────────

// Smooth interpolation curve: 6t^5 - 15t^4 + 10t^3
// This is the "improved" fade from the 2002 paper.  It ensures the gradient
// contributions join smoothly at integer lattice points — no visible seams.
static inline float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Standard linear interpolation: lerp(a, b, t) = a + t*(b-a)
static inline float lerp(float a, float b, float t) {
    return a + t * (b - a);
}

// Gradient function: maps a hash value (0-15) to one of 12 gradient vectors
// pointing to the edges and corners of a cube.  The dot product of the gradient
// with the distance vector (dx, dy, dz) gives the gradient's contribution at
// that lattice corner.
//
// The 12 gradients are the midpoints of the 12 edges of a unit cube, which
// avoids bias toward any axis.
static inline float grad(int hash, float x, float y, float z) {
    // Lower 4 bits select one of 16 gradient cases (12 real + 4 repeats).
    int h = hash & 15;
    // u is either x or y based on bit 3.
    float u = (h < 8) ? x : y;
    // v is x, y, or z depending on bits 2 and 3.
    float v = (h < 4) ? y : ((h == 12 || h == 14) ? x : z);
    // Bits 0 and 1 flip the sign of u and v respectively.
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

// ── Core noise functions ──────────────────────────────────────────────────────

// 3D Perlin noise.  Returns a value in approximately [-1, 1].
// The exact range is [-sqrt(3/4), sqrt(3/4)] ≈ [-0.866, 0.866], but in
// practice values near the extremes are rare.
static float perlin3(float x, float y, float z) {
    // Step 1: Find the integer unit cube that contains the point.
    // We use bitwise AND with 255 instead of modulo for speed — equivalent
    // since perm[] has period 256.
    int X = (int)floorf(x) & 255;
    int Y = (int)floorf(y) & 255;
    int Z = (int)floorf(z) & 255;

    // Step 2: Find the fractional offset of the point inside the cube.
    x -= floorf(x);
    y -= floorf(y);
    z -= floorf(z);

    // Step 3: Compute smooth fade curves for each axis.
    float u = fade(x), v = fade(y), w = fade(z);

    // Step 4: Hash the 8 corners of the cube using the permutation table.
    // Each corner gets a unique pseudo-random hash derived from its (X,Y,Z).
    int A  = perm[X  ] + Y;
    int AA = perm[A  ] + Z;   int AB = perm[A+1] + Z;
    int B  = perm[X+1] + Y;
    int BA = perm[B  ] + Z;   int BB = perm[B+1] + Z;

    // Step 5: Blend gradient contributions from all 8 corners.
    // Each grad() call computes the dot product of the corner's gradient
    // with the distance from the corner to our point (x,y,z).
    // lerp() interpolates between pairs of corners along each axis.
    return lerp(
        lerp(lerp(grad(perm[AA  ], x,   y,   z   ),
                  grad(perm[BA  ], x-1, y,   z   ), u),
             lerp(grad(perm[AB  ], x,   y-1, z   ),
                  grad(perm[BB  ], x-1, y-1, z   ), u), v),
        lerp(lerp(grad(perm[AA+1], x,   y,   z-1 ),
                  grad(perm[BA+1], x-1, y,   z-1 ), u),
             lerp(grad(perm[AB+1], x,   y-1, z-1 ),
                  grad(perm[BB+1], x-1, y-1, z-1 ), u), v), w);
}

// 2D noise: sample the 3D function with z=0.
static inline float perlin2(float x, float y) { return perlin3(x, y, 0.0f); }

// 1D noise: sample with y=z=0.
static inline float perlin1(float x)          { return perlin3(x, 0.0f, 0.0f); }

// ── Fractal Brownian Motion ───────────────────────────────────────────────────
//
// fbm stacks multiple octaves of noise at increasing frequencies and
// decreasing amplitudes.  The result looks like natural turbulence —
// fine detail overlaid on coarser shapes.
//
// Each octave:
//   - frequency *= lacunarity  (how much faster the next octave oscillates,
//                               typically 2.0 — each octave is twice as fine)
//   - amplitude *= gain        (how much quieter the next octave is,
//                               typically 0.5 — each octave is half as loud)
//
// The sum converges because gain < 1.  With octaves=6, lacunarity=2, gain=0.5
// the total amplitude approaches 1.0 (geometric series: 0.5+0.25+0.125+...).
static float fbm2(float x, float y, int octaves, float lacunarity, float gain) {
    float value     = 0.0f;
    float amplitude = 0.5f;    // start at 0.5 so the sum peaks near ±1
    float frequency = 1.0f;

    for (int i = 0; i < octaves; i++) {
        value     += amplitude * perlin2(x * frequency, y * frequency);
        frequency *= lacunarity;
        amplitude *= gain;
    }
    return value;
}

// ── Lua bindings ──────────────────────────────────────────────────────────────

// noise(x) / noise(x, y) / noise(x, y, z)
// Returns a float in approximately [-1, 1].
// Number of arguments determines which overload is called.
static int l_noise(lua_State* L) {
    int n = lua_gettop(L);
    float result = 0.0f;
    if (n >= 3) {
        float x = (float)luaL_checknumber(L, 1);
        float y = (float)luaL_checknumber(L, 2);
        float z = (float)luaL_checknumber(L, 3);
        result = perlin3(x, y, z);
    } else if (n == 2) {
        float x = (float)luaL_checknumber(L, 1);
        float y = (float)luaL_checknumber(L, 2);
        result = perlin2(x, y);
    } else {
        float x = (float)luaL_checknumber(L, 1);
        result = perlin1(x);
    }
    lua_pushnumber(L, result);
    return 1;
}

// fbm(x, y [, octaves [, lacunarity [, gain]]])
// Returns layered noise, approximately in [-1, 1].
// Defaults: octaves=6, lacunarity=2.0, gain=0.5
static int l_fbm(lua_State* L) {
    float x         = (float)luaL_checknumber(L, 1);
    float y         = (float)luaL_checknumber(L, 2);
    int   octaves   = (int)luaL_optinteger(L, 3, 6);
    float lacunarity= (float)luaL_optnumber(L, 4, 2.0);
    float gain      = (float)luaL_optnumber(L, 5, 0.5);

    // Clamp octaves to a sane range — more than ~12 adds no visible detail
    // and wastes CPU.
    if (octaves < 1)  octaves = 1;
    if (octaves > 16) octaves = 16;

    lua_pushnumber(L, fbm2(x, y, octaves, lacunarity, gain));
    return 1;
}

void lua_noise::register_all(lua_State* L) {
    lua_register(L, "noise", l_noise);
    lua_register(L, "fbm",   l_fbm);
}
