// lua_3d.cpp
// CPU-side 3D perspective projection and wireframe shape drawing.
//
// Architecture
// ────────────
// This module maintains two module-level matrices:
//   g_view  — transforms world-space points into camera/eye space
//   g_proj  — applies perspective projection (maps eye space → clip space)
//
// When a shape is drawn, each world-space vertex is transformed by:
//   clip = g_proj * g_view * world_position
//
// Then divided by clip.w (perspective divide) to get Normalised Device
// Coordinates (NDC, -1 to +1 on each axis), then converted to pixel space.
//
// The result is passed to Renderer::draw_line, which goes through the normal
// CPU transform stack — push/translate/rotate/scale still apply on top.
//
// Coordinate system
// ─────────────────
// Right-handed, Y-up:
//   +X = right
//   +Y = up
//   +Z = toward the viewer (camera looks in the -Z direction)
//
// This matches the standard OpenGL convention.

#include "lua_3d.h"
#include "lua_bindings.h"
#include "renderer.h"

#include <cmath>
#include <cstring>
#include <cstdio>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ── Mat4 ──────────────────────────────────────────────────────────────────────
// Row-major 4×4 matrix.  m[row*4 + col].
// We do all transforms on the CPU; only the final 2-D line endpoints go to the GPU.

struct Mat4 {
    float m[16];

    static Mat4 identity() {
        Mat4 r;
        memset(r.m, 0, sizeof(r.m));
        r.m[0]=1; r.m[5]=1; r.m[10]=1; r.m[15]=1;
        return r;
    }

    // Standard perspective projection matrix (right-handed, maps z-range to [-1,1]).
    // fov_y  — full vertical field of view in radians
    // aspect — viewport width / height
    // near   — near clip plane distance (positive)
    // far    — far  clip plane distance (positive, > near)
    static Mat4 perspective(float fov_y, float aspect, float near, float far) {
        float f  = 1.0f / tanf(fov_y * 0.5f);  // cotangent of half-FOV
        float nf = near - far;                   // negative (near < far)
        Mat4 r;
        memset(r.m, 0, sizeof(r.m));
        r.m[0]  = f / aspect;
        r.m[5]  = f;
        r.m[10] = (far + near) / nf;
        r.m[11] = 2.0f * far * near / nf;
        r.m[14] = -1.0f;  // perspective divide: clip.w = -view_z
        return r;
    }

    // LookAt view matrix.
    // Constructs a basis from (eye→target) and world-up = (0,1,0), then
    // encodes the inverse camera transform so world points map to camera space.
    static Mat4 look_at(float ex, float ey, float ez,
                        float tx, float ty, float tz) {
        // Forward direction (INTO the scene, toward target)
        float fx = tx-ex, fy = ty-ey, fz = tz-ez;
        float fl = sqrtf(fx*fx + fy*fy + fz*fz);
        if (fl < 1e-6f) return identity();
        fx/=fl; fy/=fl; fz/=fl;

        // Right = cross(forward, world_up (0,1,0))
        float rx = fy*0 - fz*1;   // = -fz
        float ry = fz*0 - fx*0;   // = 0
        float rz = fx*1 - fy*0;   // = fx
        // Expanded: right = (fy*uz - fz*uy, fz*ux - fx*uz, fx*uy - fy*ux)
        // with up=(0,1,0): right = (-fz, 0, fx)
        rx = -fz; ry = 0.0f; rz = fx;
        float rl = sqrtf(rx*rx + ry*ry + rz*rz);
        if (rl < 1e-6f) {
            // Camera is looking straight up or down — degenerate case.
            // Fall back to a stable right vector.
            rx = 1; ry = 0; rz = 0; rl = 1;
        }
        rx/=rl; ry/=rl; rz/=rl;

        // Corrected up = cross(right, forward)
        float ux = ry*fz - rz*fy;
        float uy = rz*fx - rx*fz;
        float uz = rx*fy - ry*fx;
        // (ux,uy,uz should already be unit length since right⊥forward)

        Mat4 v;
        memset(v.m, 0, sizeof(v.m));
        // Row 0: right vector
        v.m[0] = rx; v.m[1] = ry; v.m[2] = rz;
        v.m[3] = -(rx*ex + ry*ey + rz*ez);
        // Row 1: corrected up vector
        v.m[4] = ux; v.m[5] = uy; v.m[6] = uz;
        v.m[7] = -(ux*ex + uy*ey + uz*ez);
        // Row 2: -forward (camera looks in -Z, so we negate forward)
        v.m[8] = -fx; v.m[9] = -fy; v.m[10] = -fz;
        v.m[11] = fx*ex + fy*ey + fz*ez;
        // Row 3: homogeneous
        v.m[15] = 1.0f;
        return v;
    }

    // Rotation matrices around each axis (right-handed, radians).
    static Mat4 rot_x(float a) {
        float c = cosf(a), s = sinf(a);
        Mat4 r = identity();
        r.m[5] = c; r.m[6] = -s;
        r.m[9] = s; r.m[10] = c;
        return r;
    }
    static Mat4 rot_y(float a) {
        float c = cosf(a), s = sinf(a);
        Mat4 r = identity();
        r.m[0] = c; r.m[2] = s;
        r.m[8] = -s; r.m[10] = c;
        return r;
    }
    static Mat4 rot_z(float a) {
        float c = cosf(a), s = sinf(a);
        Mat4 r = identity();
        r.m[0] = c; r.m[1] = -s;
        r.m[4] = s; r.m[5] = c;
        return r;
    }

    static Mat4 translate(float tx, float ty, float tz) {
        Mat4 r = identity();
        r.m[3] = tx; r.m[7] = ty; r.m[11] = tz;
        return r;
    }

    // Matrix multiply: this * o  (applies o first, then this)
    Mat4 operator*(const Mat4& o) const {
        Mat4 res;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[i*4+k] * o.m[k*4+j];
                res.m[i*4+j] = s;
            }
        return res;
    }

    // Transform a homogeneous 4D vector.
    void xform(float x, float y, float z, float w,
               float& ox, float& oy, float& oz, float& ow) const {
        ox = m[0]*x + m[1]*y + m[2]*z  + m[3]*w;
        oy = m[4]*x + m[5]*y + m[6]*z  + m[7]*w;
        oz = m[8]*x + m[9]*y + m[10]*z + m[11]*w;
        ow = m[12]*x+ m[13]*y+ m[14]*z + m[15]*w;
    }
};

// ── Module state ──────────────────────────────────────────────────────────────

static Mat4 g_view;          // current view matrix
static Mat4 g_proj;          // current projection matrix
static Mat4 g_vp;            // cached g_proj * g_view (recomputed when dirty)
static bool g_vp_dirty = true;

static void set_defaults() {
    // Default: camera at (0, 3, 6) looking at the origin, 60° FOV, 16:9.
    g_view = Mat4::look_at(0, 3, 6, 0, 0, 0);
    g_proj = Mat4::perspective(1.0472f, 16.0f/9.0f, 0.1f, 1000.0f);
    g_vp_dirty = true;
}

// ── Projection helper ─────────────────────────────────────────────────────────
// Transforms a world-space point through the VP matrix, performs perspective
// divide, and converts to pixel coordinates.
// Returns false if the point is behind or on the camera plane (clip.w ≤ 0).
static bool project(float wx, float wy, float wz,
                    float sw, float sh,
                    float& sx, float& sy) {
    if (g_vp_dirty) {
        g_vp = g_proj * g_view;
        g_vp_dirty = false;
    }

    float cx, cy, cz, cw;
    g_vp.xform(wx, wy, wz, 1.0f, cx, cy, cz, cw);

    // cw = -view_z; positive means the point is in front of the camera.
    if (cw <= 0.0f) return false;

    float inv_w = 1.0f / cw;
    float ndcx  = cx * inv_w;   // [-1, 1]
    float ndcy  = cy * inv_w;   // [-1, 1]

    // NDC to pixel: +Y is up in NDC, down on screen — so we flip Y.
    sx = (ndcx + 1.0f) * 0.5f * sw;
    sy = (1.0f - ndcy) * 0.5f * sh;
    return true;
}

// Helper: draw a line between two world-space points if both are visible.
// If one point is behind the camera, the edge is skipped entirely
// (no clipping against the near plane — simple but sufficient for wireframes).
static void wire_edge(Renderer* r,
                      float ax, float ay, float az,
                      float bx, float by, float bz,
                      float sw, float sh) {
    float sax, say, sbx, sby;
    if (project(ax, ay, az, sw, sh, sax, say) &&
        project(bx, by, bz, sw, sh, sbx, sby)) {
        r->draw_line(sax, say, sbx, sby);
    }
}

// ── Lua API ───────────────────────────────────────────────────────────────────

static int l_camera_3d(lua_State* L) {
    float ex = (float)luaL_checknumber(L, 1);
    float ey = (float)luaL_checknumber(L, 2);
    float ez = (float)luaL_checknumber(L, 3);
    float tx = (float)luaL_checknumber(L, 4);
    float ty = (float)luaL_checknumber(L, 5);
    float tz = (float)luaL_checknumber(L, 6);
    g_view    = Mat4::look_at(ex, ey, ez, tx, ty, tz);
    g_vp_dirty = true;
    return 0;
}

static int l_perspective_3d(lua_State* L) {
    float fov  = (float)luaL_checknumber(L, 1);
    float near = (float)luaL_optnumber(L, 2, 0.1);
    float far  = (float)luaL_optnumber(L, 3, 1000.0);

    // Read current screen dimensions for automatic aspect ratio.
    lua_getglobal(L, "screen_width");
    lua_getglobal(L, "screen_height");
    float sw = (float)lua_tonumber(L, -2);
    float sh = (float)lua_tonumber(L, -1);
    lua_pop(L, 2);
    float aspect = (sh > 0.0f) ? (sw / sh) : (16.0f/9.0f);

    g_proj    = Mat4::perspective(fov, aspect, near, far);
    g_vp_dirty = true;
    return 0;
}

static int l_reset_3d(lua_State* L) {
    (void)L;
    set_defaults();
    return 0;
}

// project_3d(wx, wy, wz)  →  sx, sy  (or nothing if behind camera)
static int l_project_3d(lua_State* L) {
    float wx = (float)luaL_checknumber(L, 1);
    float wy = (float)luaL_checknumber(L, 2);
    float wz = (float)luaL_checknumber(L, 3);

    lua_getglobal(L, "screen_width");
    lua_getglobal(L, "screen_height");
    float sw = (float)lua_tonumber(L, -2);
    float sh = (float)lua_tonumber(L, -1);
    lua_pop(L, 2);

    float sx, sy;
    if (!project(wx, wy, wz, sw, sh, sx, sy)) return 0;  // nil, nil

    lua_pushnumber(L, sx);
    lua_pushnumber(L, sy);
    return 2;
}

// draw_wire_cube(cx, cy, cz, size, rx, ry, rz)
// Draws a wireframe box centred at (cx,cy,cz) with edge length `size`.
// rx, ry, rz are Euler rotation angles in radians applied in Y→X→Z order.
static int l_draw_wire_cube(lua_State* L) {
    float cx   = (float)luaL_checknumber(L, 1);
    float cy   = (float)luaL_checknumber(L, 2);
    float cz   = (float)luaL_checknumber(L, 3);
    float size = (float)luaL_checknumber(L, 4);
    float rx   = (float)luaL_optnumber(L, 5, 0.0);
    float ry   = (float)luaL_optnumber(L, 6, 0.0);
    float rz   = (float)luaL_optnumber(L, 7, 0.0);

    Renderer* r = lua_bindings::get_renderer(L);
    if (!r) return 0;

    lua_getglobal(L, "screen_width");
    lua_getglobal(L, "screen_height");
    float sw = (float)lua_tonumber(L, -2);
    float sh = (float)lua_tonumber(L, -1);
    lua_pop(L, 2);

    // Model matrix: rotate then translate to world position.
    // Rotation order Ry*Rx*Rz (yaw → pitch → roll).
    Mat4 model = Mat4::translate(cx, cy, cz)
               * Mat4::rot_y(ry)
               * Mat4::rot_x(rx)
               * Mat4::rot_z(rz);

    // Temporarily append model to the VP so we can call wire_edge with model-space coords.
    // Save/restore g_vp state so the cache remains valid afterwards.
    if (g_vp_dirty) { g_vp = g_proj * g_view; g_vp_dirty = false; }
    Mat4 saved_vp = g_vp;
    g_vp = g_vp * model;

    // 8 unit-cube vertices (±1 on each axis) scaled by half-size.
    float h = size * 0.5f;
    float v[8][3] = {
        {-h,-h,-h}, { h,-h,-h}, { h, h,-h}, {-h, h,-h},  // front face (z = -h)
        {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h},  // back face  (z = +h)
    };

    // 12 edges of the cube: 4 front, 4 back, 4 connecting sides.
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},   // front face
        {4,5},{5,6},{6,7},{7,4},   // back face
        {0,4},{1,5},{2,6},{3,7},   // connecting edges
    };

    for (auto& e : edges)
        wire_edge(r, v[e[0]][0],v[e[0]][1],v[e[0]][2],
                     v[e[1]][0],v[e[1]][1],v[e[1]][2], sw, sh);

    // Restore original VP cache.
    g_vp = saved_vp;
    return 0;
}

// draw_wire_sphere(cx, cy, cz, r [, lat_segs [, lon_segs]])
// lat_segs — number of latitude circles (default 8)
// lon_segs — number of longitude arcs   (default 16)
static int l_draw_wire_sphere(lua_State* L) {
    float cx      = (float)luaL_checknumber(L, 1);
    float cy      = (float)luaL_checknumber(L, 2);
    float cz      = (float)luaL_checknumber(L, 3);
    float rad     = (float)luaL_checknumber(L, 4);
    int   lat     = (int)luaL_optinteger(L, 5, 8);
    int   lon     = (int)luaL_optinteger(L, 6, 16);

    if (lat < 2)  lat = 2;
    if (lon < 3)  lon = 3;
    if (lat > 64) lat = 64;
    if (lon > 64) lon = 64;

    Renderer* r = lua_bindings::get_renderer(L);
    if (!r) return 0;

    lua_getglobal(L, "screen_width");
    lua_getglobal(L, "screen_height");
    float sw = (float)lua_tonumber(L, -2);
    float sh = (float)lua_tonumber(L, -1);
    lua_pop(L, 2);

    // Latitude circles: rings at phi = i*π/(lat+1) for i = 1..lat.
    // phi = 0 → top pole, phi = π → bottom pole (both skipped — just points).
    for (int i = 1; i <= lat; i++) {
        float phi = (float)i * 3.14159265f / (float)(lat + 1);
        float ring_r = rad * sinf(phi);   // radius of this latitude circle
        float ring_y = cy + rad * cosf(phi);  // y height of this circle

        for (int j = 0; j < lon; j++) {
            float theta0 = (float) j      * 6.28318530f / (float)lon;
            float theta1 = (float)(j + 1) * 6.28318530f / (float)lon;
            wire_edge(r,
                cx + ring_r * cosf(theta0), ring_y, cz + ring_r * sinf(theta0),
                cx + ring_r * cosf(theta1), ring_y, cz + ring_r * sinf(theta1),
                sw, sh);
        }
    }

    // Longitude arcs: great-circle halves from top pole to bottom pole.
    // Divided into (lat+1) segments so they pass through the latitude circles.
    for (int j = 0; j < lon; j++) {
        float theta = (float)j * 6.28318530f / (float)lon;
        float ct = cosf(theta), st = sinf(theta);

        for (int i = 0; i <= lat; i++) {
            float phi0 = (float) i      * 3.14159265f / (float)(lat + 1);
            float phi1 = (float)(i + 1) * 3.14159265f / (float)(lat + 1);
            wire_edge(r,
                cx + rad*sinf(phi0)*ct, cy + rad*cosf(phi0), cz + rad*sinf(phi0)*st,
                cx + rad*sinf(phi1)*ct, cy + rad*cosf(phi1), cz + rad*sinf(phi1)*st,
                sw, sh);
        }
    }

    return 0;
}

// draw_wire_grid(size, divs [, y])
// Draws a flat grid in the XZ plane at height y (default 0).
// size  — total side length
// divs  — number of subdivisions per side
static int l_draw_wire_grid(lua_State* L) {
    float size = (float)luaL_checknumber(L, 1);
    int   divs = (int)luaL_checkinteger(L, 2);
    float gy   = (float)luaL_optnumber(L, 3, 0.0);

    if (divs < 1)  divs = 1;
    if (divs > 64) divs = 64;

    Renderer* r = lua_bindings::get_renderer(L);
    if (!r) return 0;

    lua_getglobal(L, "screen_width");
    lua_getglobal(L, "screen_height");
    float sw = (float)lua_tonumber(L, -2);
    float sh = (float)lua_tonumber(L, -1);
    lua_pop(L, 2);

    float half = size * 0.5f;
    float step = size / (float)divs;

    // Lines parallel to Z axis (varying X)
    for (int i = 0; i <= divs; i++) {
        float x = -half + i * step;
        wire_edge(r, x, gy, -half, x, gy, half, sw, sh);
    }
    // Lines parallel to X axis (varying Z)
    for (int i = 0; i <= divs; i++) {
        float z = -half + i * step;
        wire_edge(r, -half, gy, z, half, gy, z, sw, sh);
    }

    return 0;
}

// ── Registration ──────────────────────────────────────────────────────────────

void lua_3d::register_all(lua_State* L) {
    set_defaults();
    lua_register(L, "camera_3d",       l_camera_3d);
    lua_register(L, "perspective_3d",  l_perspective_3d);
    lua_register(L, "reset_3d",        l_reset_3d);
    lua_register(L, "project_3d",      l_project_3d);
    lua_register(L, "draw_wire_cube",  l_draw_wire_cube);
    lua_register(L, "draw_wire_sphere",l_draw_wire_sphere);
    lua_register(L, "draw_wire_grid",  l_draw_wire_grid);
}
