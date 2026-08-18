#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef __cplusplus
}
#endif

#include "texture_loader.h"

// Basic image struct
struct Image {
    C3D_Tex* texture;
    int width;
    int height;
    Tex3DS_SubTexture default_subtex;
    C2D_ImageTint default_tint;
};

// Global draw color (set by love.graphics.setColor)
static float g_drawR = 1.0f, g_drawG = 1.0f, g_drawB = 1.0f, g_drawA = 1.0f;

#define MAX_SUBTEX_PER_FRAME 8192
static Tex3DS_SubTexture g_subtex_pool[MAX_SUBTEX_PER_FRAME];
static int g_subtex_count = 0;
static float g_currentZ = 0.0f;

// Stereoscopic 3D (glasses-free, 3DS parallax barrier): g_eyeParallax is set
// by the main loop per eye (+slider for the left eye, -slider for the right,
// 0 when the 3D slider is down), and g_parallaxLayer is the per-layer
// game-pixel parallax the Lua draw publishes via love.graphics._parallax
// (tiles recess, sprites pop, UI/borders sit at the screen plane).  Every
// image/batch/rect draw adds g_eyeParallax * g_parallaxLayer to its X so the
// two eye views diverge and the barrier fuses them into depth.  At slider 0
// the offset is 0 for both eyes and the frame renders exactly as before.
float g_eyeParallax = 0.0f;
static float g_parallaxLayer = 0.0f;

// Per-frame draw diagnostics (reset by the main loop each frame).  These tell
// us how many C2D_DrawImageAt calls a frame issues and how long the SpriteBatch
// loops take, so the real draw bottleneck is visible in the game log instead
// of guessed at.
unsigned g_dbg_batch_calls = 0;    // love.graphics.draw(SpriteBatch) calls
unsigned g_dbg_batch_entries = 0;  // total C2D_DrawImageAt issued for batches
unsigned g_dbg_single_calls = 0;   // love.graphics.draw(image[,quad]) calls
u64 g_dbg_batch_ms = 0;            // ms spent inside the batch draw loops
u64 g_dbg_draw_cpp_ms = 0;         // total ms spent inside l_graphics_draw (all paths)
// texture-switch / GPU-draw-call estimate: C2D flushes its internal vertex
// buffer whenever the bound texture changes, so one switch == one
// C3D_DrawArrays == one GPU draw call.  If this number is large (tens), the
// ~11ms gpuWait is draw-call/texture-switch bound, not fill-rate bound.
C3D_Tex* g_last_tex = NULL;
unsigned g_dbg_tex_switches = 0;

static int l_graphics_clear(lua_State* L) {
    g_currentZ = 0.0f;
    g_subtex_count = 0;
    return 0;
}

static int l_graphics_setColor(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushinteger(L, 1); lua_gettable(L, 1); g_drawR = (float)luaL_optnumber(L, -1, 1.0); lua_pop(L, 1);
        lua_pushinteger(L, 2); lua_gettable(L, 1); g_drawG = (float)luaL_optnumber(L, -1, 1.0); lua_pop(L, 1);
        lua_pushinteger(L, 3); lua_gettable(L, 1); g_drawB = (float)luaL_optnumber(L, -1, 1.0); lua_pop(L, 1);
        lua_pushinteger(L, 4); lua_gettable(L, 1); g_drawA = (float)luaL_optnumber(L, -1, 1.0); lua_pop(L, 1);
    } else {
        g_drawR = (float)luaL_optnumber(L, 1, 1.0);
        g_drawG = (float)luaL_optnumber(L, 2, 1.0);
        g_drawB = (float)luaL_optnumber(L, 3, 1.0);
        g_drawA = (float)luaL_optnumber(L, 4, 1.0);
    }
    return 0;
}

static int l_graphics_getColor(lua_State* L) {
    lua_pushnumber(L, g_drawR);
    lua_pushnumber(L, g_drawG);
    lua_pushnumber(L, g_drawB);
    lua_pushnumber(L, g_drawA);
    return 4;
}

static void get_screen_scales(lua_State* L, float& gScaleX, float& gScaleY) {
    gScaleX = 1.0f;
    gScaleY = 1.0f;
    g_parallaxLayer = 0.0f;
    lua_getglobal(L, "love");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "graphics");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "_scaleX");
            if (lua_isnumber(L, -1)) gScaleX = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "_scaleY");
            if (lua_isnumber(L, -1)) gScaleY = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "_parallax");
            if (lua_isnumber(L, -1)) g_parallaxLayer = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static float g_scissorX = 0;
static float g_scissorY = 0;
static float g_scissorW = 0;
static float g_scissorH = 0;
static bool g_scissorEnabled = false;
static bool g_scissorFlush = false;

static int l_graphics_setScissorFlush(lua_State* L) {
    g_scissorFlush = lua_toboolean(L, 1);
    return 0;
}

static int l_graphics_setScissor(lua_State* L) {
    if (g_scissorFlush) C2D_Flush();
    if (lua_isnoneornil(L, 1)) {
        g_scissorEnabled = false;
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 240, 400);
    } else {
        float x = (float)luaL_checknumber(L, 1);
        float y = (float)luaL_checknumber(L, 2);
        float w = (float)luaL_checknumber(L, 3);
        float h = (float)luaL_checknumber(L, 4);

        g_scissorX = x;
        g_scissorY = y;
        g_scissorW = w;
        g_scissorH = h;
        g_scissorEnabled = true;

        float gScaleX, gScaleY;
        get_screen_scales(L, gScaleX, gScaleY);

        float screen_x = x * gScaleX;
        float screen_y = y * gScaleY;
        float screen_w = w * gScaleX;
        float screen_h = h * gScaleY;

        u32 left = (u32)(240.0f - (screen_y + screen_h));
        u32 right = (u32)(240.0f - screen_y);
        u32 top = (u32)screen_x;
        u32 bottom = (u32)(screen_x + screen_w);

        if (left > 240) left = 0;
        if (right > 240) right = 240;
        if (top > 400) top = 0;
        if (bottom > 400) bottom = 400;

        C3D_SetScissor(GPU_SCISSOR_NORMAL, left, top, right, bottom);
    }
    return 0;
}

static int l_graphics_intersectScissor(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float w = (float)luaL_checknumber(L, 3);
    float h = (float)luaL_checknumber(L, 4);

    if (g_scissorFlush) C2D_Flush();

    if (g_scissorEnabled) {
        float right = g_scissorX + g_scissorW;
        float bottom = g_scissorY + g_scissorH;
        float new_right = x + w;
        float new_bottom = y + h;

        x = (x > g_scissorX) ? x : g_scissorX;
        y = (y > g_scissorY) ? y : g_scissorY;
        right = (new_right < right) ? new_right : right;
        bottom = (new_bottom < bottom) ? new_bottom : bottom;

        w = right - x;
        h = bottom - y;
        if (w < 0) w = 0;
        if (h < 0) h = 0;
    }
    
    lua_pushcfunction(L, l_graphics_setScissor);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    lua_call(L, 4, 0);

    lua_pushboolean(L, 1);
    return 1;
}

static int l_graphics_rectangle(lua_State* L) {
    float x = (float)luaL_optnumber(L, 2, 0);
    float y = (float)luaL_optnumber(L, 3, 0);
    float w = (float)luaL_optnumber(L, 4, 0);
    float h = (float)luaL_optnumber(L, 5, 0);

    float gScaleX, gScaleY;
    get_screen_scales(L, gScaleX, gScaleY);

    u32 color = C2D_Color32((u8)(g_drawR * 255.0f), (u8)(g_drawG * 255.0f), (u8)(g_drawB * 255.0f), (u8)(g_drawA * 255.0f));
    g_currentZ += 0.00001f;
    C2D_DrawRectSolid((x + g_eyeParallax * g_parallaxLayer) * gScaleX, y * gScaleY, g_currentZ, w * gScaleX, h * gScaleY, color);
    return 0;
}

static int l_image_getDimensions(lua_State* L) {
    Image* img = (Image*)lua_touserdata(L, 1);
    if (!img) return 0;
    lua_pushnumber(L, img->width);
    lua_pushnumber(L, img->height);
    return 2;
}

static int l_image_getWidth(lua_State* L) {
    Image* img = (Image*)lua_touserdata(L, 1);
    if (!img) return 0;
    lua_pushnumber(L, img->width);
    return 1;
}

static int l_image_getHeight(lua_State* L) {
    Image* img = (Image*)lua_touserdata(L, 1);
    if (!img) return 0;
    lua_pushnumber(L, img->height);
    return 1;
}

static int l_image_gc(lua_State* L) {
    Image* img = (Image*)lua_touserdata(L, 1);
    if (img && img->texture) {
        if (img->texture->data) {
            linearFree(img->texture->data);
        }
        free(img->texture);
        img->texture = nullptr;
    }
    return 0;
}

static int l_image_setFilter(lua_State* L) {
    return 0; // stub
}

// Resolve a game-relative path to the first existing file on the SD card or
// romfs, honoring the Lua global _activeCachePrefix (e.g. "blue/") so a
// runtime-imported cache on SD overlays the romfs root cache.  Returns 1 and
// fills `out` on success, 0 if nothing matched.
static int resolve_image_path(lua_State* L, const char* path, char* out, size_t outsz) {
    const char* prefix = NULL;
    size_t plen = 0;
    lua_getglobal(L, "_activeCachePrefix");
    if (lua_isstring(L, -1)) {
        prefix = lua_tolstring(L, -1, &plen);
    }
    lua_pop(L, 1);

    if (prefix && plen > 0) {
        snprintf(out, outsz, "sdmc:/3ds/gen1recomp3ds/%.*s%s", (int)plen, prefix, path);
        FILE* f = fopen(out, "rb");
        if (f) { fclose(f); return 1; }
    }
    snprintf(out, outsz, "sdmc:/3ds/gen1recomp3ds/%s", path);
    FILE* f = fopen(out, "rb");
    if (f) { fclose(f); return 1; }
    snprintf(out, outsz, "romfs:/%s", path);
    f = fopen(out, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static int l_graphics_newImage(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    Image* img = (Image*)lua_newuserdata(L, sizeof(Image));
    img->texture = (C3D_Tex*)malloc(sizeof(C3D_Tex));
    if (!img->texture) return luaL_error(L, "Out of memory allocating C3D_Tex");
    memset(img->texture, 0, sizeof(C3D_Tex));
    
    int w, h;
    char fullpath[384];
    if (!resolve_image_path(L, path, fullpath, sizeof(fullpath))) {
        free(img->texture);
        img->texture = nullptr;
        return luaL_error(L, "Could not load image: %s", path);
    }
    
    if (texture_load_to_tex(fullpath, img->texture, &w, &h) != 0) {
        free(img->texture);
        img->texture = nullptr;
        return luaL_error(L, "Could not load image: %s", fullpath);
    }
    
    img->width = w;
    img->height = h;
    
    if (luaL_newmetatable(L, "Image")) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, l_image_gc);
        lua_setfield(L, -2, "__gc");

        lua_pushcfunction(L, l_image_getDimensions);
        lua_setfield(L, -2, "getDimensions");

        lua_pushcfunction(L, l_image_getWidth);
        lua_setfield(L, -2, "getWidth");

        lua_pushcfunction(L, l_image_getHeight);
        lua_setfield(L, -2, "getHeight");

        lua_pushcfunction(L, l_image_setFilter);
        lua_setfield(L, -2, "setFilter");
    }
    lua_setmetatable(L, -2);
    
    return 1;
}


struct SpriteBatchEntry {
    float left, right, top, bottom; // precomputed UVs (set once at add())
    u16 sw, sh;                     // subtex pixel size
    float x, y;
    float scaleX, scaleY;
};

struct SpriteBatch {
    Image* img;
    int count;
    int capacity;
    SpriteBatchEntry* entries;
};

static int l_spritebatch_gc(lua_State* L) {
    SpriteBatch* sb = (SpriteBatch*)lua_touserdata(L, 1);
    if (sb) {
        if (sb->entries) {
            free(sb->entries);
            sb->entries = nullptr;
        }
    }
    return 0;
}

static int l_spritebatch_clear(lua_State* L) {
    SpriteBatch* sb = (SpriteBatch*)lua_touserdata(L, 1);
    if (sb) sb->count = 0;
    return 0;
}

static int l_spritebatch_setTexture(lua_State* L) {
    SpriteBatch* sb = (SpriteBatch*)lua_touserdata(L, 1);
    Image* img = (Image*)lua_touserdata(L, 2);
    if (sb && img) {
        sb->img = img;
    }
    return 0;
}

static int l_spritebatch_add(lua_State* L) {
    SpriteBatch* sb = (SpriteBatch*)lua_touserdata(L, 1);
    if (!sb) return 0;
    
    if (sb->count >= sb->capacity) {
        sb->capacity = (sb->capacity == 0) ? 128 : sb->capacity * 2;
        sb->entries = (SpriteBatchEntry*)realloc(sb->entries, sb->capacity * sizeof(SpriteBatchEntry));
    }
    
    SpriteBatchEntry& e = sb->entries[sb->count++];
    e.scaleX = 1.0f;
    e.scaleY = 1.0f;
    
    // UVs are precomputed here (once per add) so the per-frame draw loop does
    // NO float divisions -- ARM11 has no FPU, and the batch loop was doing 4
    // texture-size divisions per entry every frame (576 tiles / frame).
    C3D_Tex* tex = (sb->img && sb->img->texture) ? sb->img->texture : NULL;
    float tw = tex ? (float)tex->width : 1.0f;
    float th = tex ? (float)tex->height : 1.0f;
    
    if (lua_istable(L, 2)) {
        float qx, qy, qw, qh;
        lua_getfield(L, 2, "_x"); qx = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "_y"); qy = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "_w"); qw = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "_h"); qh = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        
        e.x = (float)luaL_optnumber(L, 3, 0);
        e.y = (float)luaL_optnumber(L, 4, 0);
        e.scaleX = (float)luaL_optnumber(L, 6, 1.0f);
        e.scaleY = (float)luaL_optnumber(L, 7, 1.0f);
        
        e.left   = (qx + 0.05f) / tw;
        e.right  = (qx + qw - 0.05f) / tw;
        e.top    = 1.0f - (qy + 0.05f) / th;
        e.bottom = 1.0f - (qy + qh - 0.05f) / th;
        e.sw = (u16)qw;
        e.sh = (u16)qh;
    } else {
        e.x = (float)luaL_optnumber(L, 2, 0);
        e.y = (float)luaL_optnumber(L, 3, 0);
        e.scaleX = (float)luaL_optnumber(L, 5, 1.0f);
        e.scaleY = (float)luaL_optnumber(L, 6, 1.0f);
        
        e.left   = 0.0f;
        e.right  = tex ? (float)sb->img->width / tw : 0.0f;
        e.top    = 1.0f;
        e.bottom = tex ? 1.0f - (float)sb->img->height / th : 1.0f;
        e.sw = tex ? (u16)sb->img->width : 0;
        e.sh = tex ? (u16)sb->img->height : 0;
    }
    return 0;
}

static int l_graphics_newSpriteBatch(lua_State* L) {
    Image* img = (Image*)lua_touserdata(L, 1);
    int size = luaL_optinteger(L, 2, 1024);
    
    SpriteBatch* sb = (SpriteBatch*)lua_newuserdata(L, sizeof(SpriteBatch));
    sb->img = img;
    sb->count = 0;
    sb->capacity = size;
    sb->entries = (SpriteBatchEntry*)malloc(size * sizeof(SpriteBatchEntry));
    
    if (luaL_newmetatable(L, "SpriteBatch")) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        
        lua_pushcfunction(L, l_spritebatch_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, l_spritebatch_clear);
        lua_setfield(L, -2, "clear");
        lua_pushcfunction(L, l_spritebatch_add);
        lua_setfield(L, -2, "add");
        lua_pushcfunction(L, l_spritebatch_setTexture);
        lua_setfield(L, -2, "setTexture");
    }
    lua_setmetatable(L, -2);
    
    return 1;
}

// ---- Canvas (render-to-texture) ------------------------------------------
// Real GPU render targets for love.graphics.newCanvas / setCanvas / renderTo.
// Each canvas wraps a C3D_Tex (linear memory) and a C3D_RenderTarget that
// the GPU renders into.  When drawn via love.graphics.draw, the canvas
// texture is blitted like any other image.

struct Canvas {
    C3D_Tex* texture;
    C3D_RenderTarget* target;
    Tex3DS_SubTexture default_subtex;
    int width;
    int height;
};

// The main render loop sets this to the current eye's screen target before
// each love.draw() call, so setCanvas(nil) knows where to switch back to.
static C3D_RenderTarget* g_screen_target = NULL;

// Saved screen scale factors, captured once when setCanvas first switches
// away from the screen.  Restored when switching back.
static float g_saved_screen_scaleX = 1.0f;
static float g_saved_screen_scaleY = 1.0f;
static bool g_canvas_active = false;

extern "C" void graphics_setScreenTarget(C3D_RenderTarget* target) {
    g_screen_target = target;
}

extern "C" void graphics_setScreenScale(float sx, float sy) {
    g_saved_screen_scaleX = sx;
    g_saved_screen_scaleY = sy;
}

// Push _scaleX / _scaleY into love.graphics so the C++ draw path picks them
// up via get_screen_scales().  Called by setCanvas / renderTo to switch
// between screen-scale (2.5x/1.667x) and canvas-scale (1:1).
static void set_lua_scale(lua_State* L, float sx, float sy) {
    lua_getglobal(L, "love");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "graphics");
        if (lua_istable(L, -1)) {
            lua_pushnumber(L, sx);
            lua_setfield(L, -2, "_scaleX");
            lua_pushnumber(L, sy);
            lua_setfield(L, -2, "_scaleY");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static int l_canvas_gc(lua_State* L) {
    Canvas* c = (Canvas*)lua_touserdata(L, 1);
    if (c) {
        if (c->target) {
            C3D_RenderTargetDelete(c->target);
            c->target = NULL;
        }
        if (c->texture) {
            if (c->texture->data) linearFree(c->texture->data);
            free(c->texture);
            c->texture = NULL;
        }
    }
    return 0;
}

static int l_canvas_getWidth(lua_State* L) {
    Canvas* c = (Canvas*)lua_touserdata(L, 1);
    lua_pushinteger(L, c ? c->width : 0);
    return 1;
}

static int l_canvas_getHeight(lua_State* L) {
    Canvas* c = (Canvas*)lua_touserdata(L, 1);
    lua_pushinteger(L, c ? c->height : 0);
    return 1;
}

static int l_canvas_getDimensions(lua_State* L) {
    Canvas* c = (Canvas*)lua_touserdata(L, 1);
    lua_pushinteger(L, c ? c->width : 0);
    lua_pushinteger(L, c ? c->height : 0);
    return 2;
}

static int l_canvas_setFilter(lua_State* L) {
    return 0; // stub: always nearest on 3DS
}

static int l_canvas_typeOf(lua_State* L) {
    const char* t = luaL_checkstring(L, 2);
    lua_pushboolean(L, t && strcmp(t, "Canvas") == 0);
    return 1;
}

static int l_canvas_release(lua_State* L) {
    return l_canvas_gc(L);
}

static int l_canvas_renderTo(lua_State* L) {
    Canvas* c = (Canvas*)lua_touserdata(L, 1);
    if (!c || !c->target) return luaL_error(L, "renderTo on invalid canvas");
    if (!lua_isfunction(L, 2)) return luaL_error(L, "renderTo requires a function");

    // Save current state
    bool was_canvas = g_canvas_active;
    float curSX, curSY;
    get_screen_scales(L, curSX, curSY);

    // Switch to canvas
    C2D_Flush();
    C2D_SceneBegin(c->target);
    g_currentZ = 0.0f;
    g_subtex_count = 0;
    g_canvas_active = true;
    set_lua_scale(L, 1.0f, 1.0f);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, c->height, c->width);

    // Call the Lua function
    lua_pushvalue(L, 2);
    lua_call(L, 0, 0);

    // Restore
    C2D_Flush();
    g_canvas_active = was_canvas;
    if (was_canvas) {
        // Was already in canvas mode: stay at 1:1
        set_lua_scale(L, 1.0f, 1.0f);
    } else {
        // Was on screen: switch back
        if (g_screen_target) {
            C2D_SceneBegin(g_screen_target);
        }
        set_lua_scale(L, curSX, curSY);
    }
    g_currentZ = 0.0f;
    g_subtex_count = 0;

    return 0;
}

static int l_graphics_newCanvas(lua_State* L) {
    int w = luaL_optinteger(L, 1, 160);
    int h = luaL_optinteger(L, 2, 144);
    if (w <= 0 || h <= 0)
        return luaL_error(L, "newCanvas: dimensions must be positive (%dx%d)", w, h);

    Canvas* c = (Canvas*)lua_newuserdata(L, sizeof(Canvas));
    memset(c, 0, sizeof(Canvas));

    c->texture = (C3D_Tex*)malloc(sizeof(C3D_Tex));
    if (!c->texture) return luaL_error(L, "Out of memory allocating Canvas texture");
    memset(c->texture, 0, sizeof(C3D_Tex));

    if (!C3D_TexInit(c->texture, (u16)w, (u16)h, GPU_RGBA8)) {
        free(c->texture);
        return luaL_error(L, "Failed to create canvas texture (%dx%d)", w, h);
    }

    c->target = C3D_RenderTargetCreateFromTex(c->texture, GPU_TEXFACE_2D, 0,
                                               C3D_DEPTHTYPE(GPU_RB_DEPTH16));
    if (!c->target) {
        C3D_TexDelete(c->texture);
        free(c->texture);
        return luaL_error(L, "Failed to create canvas render target");
    }

    c->width = w;
    c->height = h;

    // Set up the subtex for drawing this canvas as an image
    c->default_subtex.left   = 0.0f;
    c->default_subtex.top    = 1.0f;
    c->default_subtex.right  = 1.0f;
    c->default_subtex.bottom = 0.0f;
    c->default_subtex.width  = (u16)w;
    c->default_subtex.height = (u16)h;

    if (luaL_newmetatable(L, "Canvas")) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, l_canvas_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, l_canvas_gc);
        lua_setfield(L, -2, "release");
        lua_pushcfunction(L, l_canvas_getWidth);
        lua_setfield(L, -2, "getWidth");
        lua_pushcfunction(L, l_canvas_getHeight);
        lua_setfield(L, -2, "getHeight");
        lua_pushcfunction(L, l_canvas_getDimensions);
        lua_setfield(L, -2, "getDimensions");
        lua_pushcfunction(L, l_canvas_setFilter);
        lua_setfield(L, -2, "setFilter");
        lua_pushcfunction(L, l_canvas_typeOf);
        lua_setfield(L, -2, "typeOf");
        lua_pushcfunction(L, l_canvas_renderTo);
        lua_setfield(L, -2, "renderTo");
    }
    lua_setmetatable(L, -2);

    return 1;
}

static int l_graphics_setCanvas(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        // Switch back to screen
        if (g_canvas_active) {
            C2D_Flush();
            if (g_screen_target) {
                C2D_SceneBegin(g_screen_target);
            }
            g_currentZ = 0.0f;
            g_subtex_count = 0;
            g_canvas_active = false;
            set_lua_scale(L, g_saved_screen_scaleX, g_saved_screen_scaleY);
        }
    } else {
        Canvas* c = (Canvas*)lua_touserdata(L, 1);
        if (!c || !c->target) return luaL_error(L, "setCanvas: expected Canvas or nil");

        // Save screen scale on first canvas switch
        if (!g_canvas_active) {
            float gScaleX, gScaleY;
            get_screen_scales(L, gScaleX, gScaleY);
            g_saved_screen_scaleX = gScaleX;
            g_saved_screen_scaleY = gScaleY;
        }

        C2D_Flush();
        C2D_SceneBegin(c->target);
        g_currentZ = 0.0f;
        g_subtex_count = 0;
        g_canvas_active = true;
        set_lua_scale(L, 1.0f, 1.0f);
    }
    return 0;
}

static int l_graphics_draw(lua_State* L) {
    u64 dbg_t0 = osGetTime();
    void* userdata = lua_touserdata(L, 1);
    if (!userdata) { g_dbg_draw_cpp_ms += osGetTime() - dbg_t0; return 0; }
    
    int is_batch = 0;
    if (lua_getmetatable(L, 1)) {
        luaL_getmetatable(L, "SpriteBatch");
        if (lua_rawequal(L, -1, -2)) {
            is_batch = 1;
        }
        lua_pop(L, 2);
    }
    
    float gScaleX = 1.0f, gScaleY = 1.0f;
    // Also reads love.graphics._parallax into g_parallaxLayer so every image /
    // SpriteBatch draw picks up the per-layer stereoscopic 3D offset.  (Using
    // the shared helper here instead of the old inline _scaleX/_scaleY reads
    // guarantees the parallax and the scales always come from the same place.)
    get_screen_scales(L, gScaleX, gScaleY);

    if (is_batch) {
        SpriteBatch* sb = (SpriteBatch*)userdata;
        if (!sb->img || !sb->img->texture) { g_dbg_draw_cpp_ms += osGetTime() - dbg_t0; return 0; }
        
        float baseX = luaL_optnumber(L, 2, 0);
        float baseY = luaL_optnumber(L, 3, 0);
        
        C3D_TexSetFilter(sb->img->texture, GPU_NEAREST, GPU_NEAREST);
        C2D_Image c2d_img;
        c2d_img.tex = sb->img->texture;
        if (c2d_img.tex != g_last_tex) { g_dbg_tex_switches++; g_last_tex = c2d_img.tex; }
        
        u64 dbg0 = osGetTime();
        for (int i = 0; i < sb->count; i++) {
            SpriteBatchEntry& e = sb->entries[i];
            
            Tex3DS_SubTexture* subtex = &g_subtex_pool[g_subtex_count++];
            if (g_subtex_count >= MAX_SUBTEX_PER_FRAME) g_subtex_count = 0;
            
            // UVs precomputed at add() -- no divisions in this hot loop
            subtex->left   = e.left;
            subtex->right  = e.right;
            subtex->top    = e.top;
            subtex->bottom = e.bottom;
            subtex->width  = e.sw;
            subtex->height = e.sh;
            c2d_img.subtex = subtex;
            
            float drawX = baseX + e.x;
            float drawY = baseY + e.y;
            if (e.scaleX < 0) drawX += subtex->width * e.scaleX;
            if (e.scaleY < 0) drawY += subtex->height * e.scaleY;
            
            g_currentZ += 0.00001f;
            C2D_DrawImageAt(c2d_img, (drawX + g_eyeParallax * g_parallaxLayer) * gScaleX, drawY * gScaleY, g_currentZ, NULL, e.scaleX * gScaleX, e.scaleY * gScaleY);
        }
        u64 dbg1 = osGetTime();
        g_dbg_batch_ms += (dbg1 - dbg0);
        g_dbg_batch_entries += sb->count;
        g_dbg_batch_calls++;
    } else {
        Image* img = (Image*)userdata;
        if (!img->texture) { g_dbg_draw_cpp_ms += osGetTime() - dbg_t0; return 0; }
        
        float x = 0, y = 0, scaleX = 1.0f, scaleY = 1.0f;
        C3D_TexSetFilter(img->texture, GPU_NEAREST, GPU_NEAREST);
        
        C2D_Image c2d_img;
        c2d_img.tex = img->texture;
        if (c2d_img.tex != g_last_tex) { g_dbg_tex_switches++; g_last_tex = c2d_img.tex; }
        
        Tex3DS_SubTexture* subtex = &g_subtex_pool[g_subtex_count++];
        if (g_subtex_count >= MAX_SUBTEX_PER_FRAME) g_subtex_count = 0;
        
        if (lua_istable(L, 2)) {
            x = luaL_optnumber(L, 3, 0);
            y = luaL_optnumber(L, 4, 0);
            scaleX = luaL_optnumber(L, 6, 1.0f);
            scaleY = luaL_optnumber(L, 7, 1.0f);
            
            lua_getfield(L, 2, "_x"); float qx = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 2, "_y"); float qy = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 2, "_w"); float qw = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 2, "_h"); float qh = lua_tonumber(L, -1); lua_pop(L, 1);
            
            subtex->left   = (qx + 0.05f) / (float)img->texture->width;
            subtex->right  = (qx + qw - 0.05f) / (float)img->texture->width;
            subtex->top    = 1.0f - (qy + 0.05f) / (float)img->texture->height;
            subtex->bottom = 1.0f - (qy + qh - 0.05f) / (float)img->texture->height;
            subtex->width  = (u16)qw;
            subtex->height = (u16)qh;
        } else {
            x = luaL_optnumber(L, 2, 0);
            y = luaL_optnumber(L, 3, 0);
            scaleX = luaL_optnumber(L, 5, 1.0f);
            scaleY = luaL_optnumber(L, 6, 1.0f);
            
            subtex->left   = 0.0f;
            subtex->right  = (float)img->width  / (float)img->texture->width;
            subtex->top    = 1.0f;
            subtex->bottom = 1.0f - (float)img->height / (float)img->texture->height;
            subtex->width  = (u16)img->width;
            subtex->height = (u16)img->height;
        }
        
        float drawX = x;
        float drawY = y;
        if (scaleX < 0) drawX += subtex->width * scaleX;
        if (scaleY < 0) drawY += subtex->height * scaleY;
        
        c2d_img.subtex = subtex;
        g_currentZ += 0.00001f;
        g_dbg_single_calls++;
        C2D_DrawImageAt(c2d_img, (drawX + g_eyeParallax * g_parallaxLayer) * gScaleX, drawY * gScaleY, g_currentZ, NULL, scaleX * gScaleX, scaleY * gScaleY);
    }
    g_dbg_draw_cpp_ms += osGetTime() - dbg_t0;
    return 0;
}



// Launcher text via citro2d's built-in system font.  In-game text never uses
// love.graphics.print (the game renders its own glyph textures), so this only
// drives the bottom-screen launcher UI and any top-screen labels.  The stub
// script wires love.graphics.print = love.graphics._printC.
static C2D_TextBuf g_textbuf = NULL;

static int l_graphics_print(lua_State* L) {
    const char* text = NULL;
    if (lua_type(L, 1) == LUA_TSTRING) {
        text = lua_tostring(L, 1);
    } else if (lua_isnumber(L, 1)) {
        lua_pushvalue(L, 1);
        text = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    if (!text) return 0;
    float x = (float)luaL_optnumber(L, 2, 0);
    float y = (float)luaL_optnumber(L, 3, 0);
    float r = g_drawR, g = g_drawG, b = g_drawB, a = g_drawA;
    if (!lua_isnoneornil(L, 4)) {
        r = (float)luaL_checknumber(L, 4);
        g = (float)luaL_optnumber(L, 5, r);
        b = (float)luaL_optnumber(L, 6, r);
        a = (float)luaL_optnumber(L, 7, 1.0);
    }

    if (!g_textbuf) g_textbuf = C2D_TextBufNew(4096);
    if (!g_textbuf) return 0;
    C2D_TextBufClear(g_textbuf);
    C2D_Text t;
    C2D_TextParse(&t, g_textbuf, text);
    C2D_TextOptimize(&t);

    float gScaleX, gScaleY;
    get_screen_scales(L, gScaleX, gScaleY);
    u32 color = C2D_Color32((u8)(r * 255.0f), (u8)(g * 255.0f),
                            (u8)(b * 255.0f), (u8)(a * 255.0f));
    // The built-in font has a 30px glyph height; ~0.4x gives 12px-high text,
    // a good size for launcher buttons / ROM list on the 320x240 bottom.
    float scale = 0.40f;
    g_currentZ += 0.00001f;
    C2D_DrawText(&t, C2D_WithColor,
                 (x + g_eyeParallax * g_parallaxLayer) * gScaleX,
                 y * gScaleY, g_currentZ,
                 scale * gScaleX, scale * gScaleY, color);
    return 0;
}



static const luaL_Reg graphics_funcs[] = {
    {"newImage", l_graphics_newImage},
    {"newSpriteBatch", l_graphics_newSpriteBatch},
    {"draw", l_graphics_draw},
    {"setColor", l_graphics_setColor},
    {"getColor", l_graphics_getColor},
    {"rectangle", l_graphics_rectangle},
    {"clear", l_graphics_clear},
    {"setScissor", l_graphics_setScissor},
    {"setScissorFlush", l_graphics_setScissorFlush},
    {"intersectScissor", l_graphics_intersectScissor},
    {"_printC", l_graphics_print},
    {NULL, NULL}
};

extern "C" int luaopen_love_graphics(lua_State* L) {
    luaL_register(L, "love.graphics", graphics_funcs);
    return 1;
}
