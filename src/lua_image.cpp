// lua_image.cpp
// Image loading and sprite sheet Lua bindings.
//
// Images are loaded from disk via stb_image, uploaded to a GL texture, and
// wrapped in a Lua full userdata so the GC frees the texture automatically.
//
// Two Lua constructors:
//   image.load("path")                    → Image userdata
//   sprite_sheet.new("path", fw, fh)      → SpriteSheet userdata
//
// Coordinates everywhere are in pixel space (top-left origin, +Y down),
// matching every other draw call in the engine.

#include "lua_image.h"
#include "lua_bindings.h"   // for lua_bindings::get_renderer
#include "renderer.h"

// glad must come before any GL types — it loads the function pointers.
#include <glad/glad.h>

// stb_image: STB_IMAGE_IMPLEMENTATION is defined in stb_image_impl.cpp;
// here we only get the declarations.
#include "stb_image.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ── Metatable name constants ──────────────────────────────────────────────────
static const char* IMAGE_MT        = "phosphor.image";
static const char* SPRITESHEET_MT  = "phosphor.spritesheet";

// ── Image struct ──────────────────────────────────────────────────────────────
// Stored as a Lua full userdata.  tex is a GL texture object; it is deleted
// in __gc so the GPU resource is freed when Lua collects the object.

struct Image {
    int          width;
    int          height;
    unsigned int tex;    // GLuint — unsigned int matches renderer.h convention
};

// ── SpriteSheet struct ────────────────────────────────────────────────────────
// Embeds the texture data directly (no separate Image object) so there are no
// lifetime dependencies between two userdata objects.

struct SpriteSheet {
    int          img_w;      // full texture width  in pixels
    int          img_h;      // full texture height in pixels
    unsigned int tex;        // GLuint
    int          frame_w;    // one frame's width  in pixels
    int          frame_h;    // one frame's height in pixels
    int          cols;       // frames per row
    int          rows;       // frames per column
    int          total;      // cols * rows — total number of frames
};

// ── Shared helper: load image file → GL texture ───────────────────────────────
//
// stb_image loads the file into a CPU buffer; we then create a GL texture and
// upload the pixels.  The CPU buffer is freed immediately after upload — the
// GPU holds the only copy.
//
// stbi_set_flip_vertically_on_load(1) makes row 0 of the file map to v=1 in
// the texture (OpenGL convention: v=0 is the bottom).  Our image shader
// accounts for this so pixel y=0 displays the top of the image, as expected.
//
// Returns 0 (invalid) on failure and sets *w, *h to 0.
static unsigned int load_texture(const char* path, int* w, int* h) {
    // Force RGBA so every image has 4 channels — simplifies the GL upload.
    stbi_set_flip_vertically_on_load(1);
    int channels = 0;
    unsigned char* data = stbi_load(path, w, h, &channels, 4);
    if (!data) {
        fprintf(stderr, "image: failed to load '%s': %s\n",
                path, stbi_failure_reason());
        *w = *h = 0;
        return 0;
    }

    // Create a 2D texture and upload the RGBA pixels.
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // GL_LINEAR gives smooth scaling; GL_NEAREST would give crisp pixel art.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Clamp to edge so sub-region draws don't bleed across the texture border.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Upload: internal format GL_RGBA8 (8 bits per channel, stored on GPU),
    // source format GL_RGBA (4 channels, unsigned bytes).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, *w, *h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);   // CPU copy no longer needed

    return tex;
}

// ── Image Lua methods ─────────────────────────────────────────────────────────

static Image* check_image(lua_State* L) {
    return (Image*)luaL_checkudata(L, 1, IMAGE_MT);
}

// img:draw(x, y [, w, h [, angle]])
// Draws the full image at pixel position (x, y).
// w and h default to the image's pixel dimensions (1:1 pixel size).
// angle (optional, radians) rotates the image around its centre.  Default 0.
//
// NOTE: img:draw() uses draw_image() which bypasses the CPU transform matrix.
// push/translate/rotate/scale do NOT affect image draws — use angle instead.
static int l_image_draw(lua_State* L) {
    Image*    img   = check_image(L);
    float     x     = (float)luaL_checknumber(L, 2);
    float     y     = (float)luaL_checknumber(L, 3);
    float     w     = (float)luaL_optnumber(L, 4, (double)img->width);
    float     h     = (float)luaL_optnumber(L, 5, (double)img->height);
    float     angle = (float)luaL_optnumber(L, 6, 0.0);
    Renderer* r     = lua_bindings::get_renderer(L);
    if (r && img->tex)
        r->draw_image(img->tex, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, angle);
    return 0;
}

// img:draw_region(x, y, w, h, src_x, src_y, src_w, src_h [, angle])
// Draws a rectangular sub-region of the image.
// All src_* coordinates are in the image's own pixel space (top-left origin).
// angle (optional, radians) rotates the result around its dest centre.  Default 0.
static int l_image_draw_region(lua_State* L) {
    Image*    img   = check_image(L);
    float     x     = (float)luaL_checknumber(L, 2);
    float     y     = (float)luaL_checknumber(L, 3);
    float     w     = (float)luaL_checknumber(L, 4);
    float     h     = (float)luaL_checknumber(L, 5);
    float     sx    = (float)luaL_checknumber(L, 6);
    float     sy    = (float)luaL_checknumber(L, 7);
    float     sw    = (float)luaL_checknumber(L, 8);
    float     sh    = (float)luaL_checknumber(L, 9);
    float     angle = (float)luaL_optnumber(L, 10, 0.0);
    Renderer* r     = lua_bindings::get_renderer(L);

    if (!r || !img->tex) return 0;

    // Convert pixel sub-rect to UV coordinates [0..1].
    // stb_image was loaded with flip, so v=1 is the TOP of the image file.
    // pixel row sy   → v = 1 - sy/img_h           (top of region)
    // pixel row sy+sh→ v = 1 - (sy+sh)/img_h       (bottom of region)
    float tw = (float)img->width;
    float th = (float)img->height;
    float u0 = sx / tw;
    float u1 = (sx + sw) / tw;
    float v0 = 1.0f - (sy + sh) / th;   // bottom UV (drawn at t.y=0 in shader)
    float v1 = 1.0f - sy / th;          // top    UV (drawn at t.y=1 in shader)

    r->draw_image(img->tex, x, y, w, h, u0, v0, u1, v1, angle);
    return 0;
}

// img:width() / img:height()
static int l_image_width(lua_State* L) {
    lua_pushinteger(L, check_image(L)->width);
    return 1;
}
static int l_image_height(lua_State* L) {
    lua_pushinteger(L, check_image(L)->height);
    return 1;
}

// __gc: delete the GL texture when the userdata is collected.
static int l_image_gc(lua_State* L) {
    Image* img = (Image*)luaL_checkudata(L, 1, IMAGE_MT);
    if (img->tex) { glDeleteTextures(1, &img->tex); img->tex = 0; }
    return 0;
}

static int l_image_tostring(lua_State* L) {
    Image* img = check_image(L);
    lua_pushfstring(L, "image(%d x %d)", img->width, img->height);
    return 1;
}

// image.load("path") → Image userdata
static int l_image_load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);

    Image* img = (Image*)lua_newuserdata(L, sizeof(Image));
    img->tex   = 0;
    img->width = img->height = 0;

    luaL_setmetatable(L, IMAGE_MT);

    // Load after setting the metatable so __gc runs even if load_texture fails.
    int w = 0, h = 0;
    img->tex    = load_texture(path, &w, &h);
    img->width  = w;
    img->height = h;

    if (!img->tex)
        luaL_error(L, "image.load: could not load '%s'", path);

    return 1;
}

// ── SpriteSheet Lua methods ───────────────────────────────────────────────────

static SpriteSheet* check_sheet(lua_State* L) {
    return (SpriteSheet*)luaL_checkudata(L, 1, SPRITESHEET_MT);
}

// sheet:draw(frame_idx, x, y [, w, h [, angle]])
// frame_idx is 1-based.  Frames are ordered left-to-right, then top-to-bottom —
// the same reading order as most sprite sheet packers produce.
// w and h default to frame_w and frame_h so the frame is drawn at 1:1 pixel size.
// angle (optional, radians) rotates the frame around its dest centre.  Default 0.
static int l_sheet_draw(lua_State* L) {
    SpriteSheet* s     = check_sheet(L);
    int          idx   = (int)luaL_checkinteger(L, 2) - 1;   // convert 1→0 index
    float        x     = (float)luaL_checknumber(L, 3);
    float        y     = (float)luaL_checknumber(L, 4);
    float        dw    = (float)luaL_optnumber(L, 5, (double)s->frame_w);
    float        dh    = (float)luaL_optnumber(L, 6, (double)s->frame_h);
    float        angle = (float)luaL_optnumber(L, 7, 0.0);
    Renderer*    r     = lua_bindings::get_renderer(L);

    if (!r || !s->tex) return 0;

    // Clamp frame index to valid range so out-of-bounds doesn't crash.
    if (idx < 0)        idx = 0;
    if (idx >= s->total) idx = s->total - 1;

    // Convert frame index to grid position.
    int col = idx % s->cols;
    int row = idx / s->cols;

    // Compute pixel origin of this frame in the source texture.
    float sx = (float)(col * s->frame_w);
    float sy = (float)(row * s->frame_h);
    float sw = (float)s->frame_w;
    float sh = (float)s->frame_h;

    // Convert to UV (with stbi vertical flip: v=1 is top of file).
    float tw = (float)s->img_w;
    float th = (float)s->img_h;
    float u0 = sx / tw;
    float u1 = (sx + sw) / tw;
    float v0 = 1.0f - (sy + sh) / th;
    float v1 = 1.0f - sy / th;

    r->draw_image(s->tex, x, y, dw, dh, u0, v0, u1, v1, angle);
    return 0;
}

static int l_sheet_frame_count(lua_State* L) {
    lua_pushinteger(L, check_sheet(L)->total);
    return 1;
}
static int l_sheet_cols(lua_State* L) {
    lua_pushinteger(L, check_sheet(L)->cols);
    return 1;
}
static int l_sheet_rows(lua_State* L) {
    lua_pushinteger(L, check_sheet(L)->rows);
    return 1;
}

static int l_sheet_gc(lua_State* L) {
    SpriteSheet* s = (SpriteSheet*)luaL_checkudata(L, 1, SPRITESHEET_MT);
    if (s->tex) { glDeleteTextures(1, &s->tex); s->tex = 0; }
    return 0;
}

static int l_sheet_tostring(lua_State* L) {
    SpriteSheet* s = check_sheet(L);
    lua_pushfstring(L, "sprite_sheet(%d frames, %d x %d px each)",
                    s->total, s->frame_w, s->frame_h);
    return 1;
}

// sprite_sheet.new("path", frame_w, frame_h) → SpriteSheet userdata
static int l_sheet_new(lua_State* L) {
    const char* path   = luaL_checkstring(L, 1);
    int         fw     = (int)luaL_checkinteger(L, 2);
    int         fh     = (int)luaL_checkinteger(L, 3);

    luaL_argcheck(L, fw >= 1, 2, "frame_w must be >= 1");
    luaL_argcheck(L, fh >= 1, 3, "frame_h must be >= 1");

    SpriteSheet* s = (SpriteSheet*)lua_newuserdata(L, sizeof(SpriteSheet));
    memset(s, 0, sizeof(SpriteSheet));
    s->frame_w = fw;
    s->frame_h = fh;

    luaL_setmetatable(L, SPRITESHEET_MT);

    int w = 0, h = 0;
    s->tex   = load_texture(path, &w, &h);
    s->img_w = w;
    s->img_h = h;

    if (!s->tex)
        luaL_error(L, "sprite_sheet.new: could not load '%s'", path);

    // How many frames fit in the sheet?  Integer division — any partial frame
    // at the right/bottom edge is ignored.
    s->cols  = (fw > 0) ? w / fw : 0;
    s->rows  = (fh > 0) ? h / fh : 0;
    s->total = s->cols * s->rows;

    if (s->total < 1)
        luaL_error(L, "sprite_sheet.new: no complete frames fit in %dx%d image "
                      "with frame size %dx%d", w, h, fw, fh);

    return 1;
}

// ── Registration ──────────────────────────────────────────────────────────────

void lua_image::register_all(lua_State* L) {
    // ── image ────────────────────────────────────────────────────────────────
    static const luaL_Reg image_methods[] = {
        { "draw",        l_image_draw        },
        { "draw_region", l_image_draw_region },
        { "width",       l_image_width       },
        { "height",      l_image_height      },
        { nullptr, nullptr }
    };

    luaL_newmetatable(L, IMAGE_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, image_methods, 0);
    lua_pushcfunction(L, l_image_gc);       lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_image_tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_image_load);
    lua_setfield(L, -2, "load");
    lua_setglobal(L, "image");

    // ── sprite_sheet ─────────────────────────────────────────────────────────
    static const luaL_Reg sheet_methods[] = {
        { "draw",        l_sheet_draw        },
        { "frame_count", l_sheet_frame_count },
        { "cols",        l_sheet_cols        },
        { "rows",        l_sheet_rows        },
        { nullptr, nullptr }
    };

    luaL_newmetatable(L, SPRITESHEET_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, sheet_methods, 0);
    lua_pushcfunction(L, l_sheet_gc);       lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_sheet_tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_sheet_new);
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "sprite_sheet");
}
