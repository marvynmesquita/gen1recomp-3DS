// love.image for the 3DS (ROM importer only).
//
// The engine keeps love.image.newImageData deliberately `false` on the 3DS
// so the slow SGB / palette-composition paths in SpriteRenderer,
// TileRenderer, BattleState & co stay gated OFF (see the note in
// BattleState.lua).  The ROM importer, however, needs a real ImageData, so
// this module exposes it under love.image._newImageData + a full ImageData
// metatable (setPixel/getPixel/mapPixel/encode).  ImageWriter.lua picks it
// on the 3DS via a small shim; on desktop it keeps using love.image.
//
// ImageData here is a plain RGBA8 buffer, row 0 = top (natural order), and
// setPixel/getPixel follow LÖVE's [0,1] float convention (the same one
// LovePotion's setPixelRGBA8 uses).  encode() does NOT write a PNG (there is
// no PNG encoder on the 3DS build): it writes the raw "GR1T" texture format
// that texture_loader.cpp understands, which is far faster to produce and
// load on the console.

#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

struct ImageData {
    int width, height;
    uint8_t* pixels; // RGBA8, width*height*4, row 0 = top, pixel = R,G,B,A
};

static ImageData* checkImageData(lua_State* L, int idx) {
    return (ImageData*)luaL_checkudata(L, idx, "ImageData");
}

static int id_gc(lua_State* L) {
    ImageData* id = (ImageData*)lua_touserdata(L, 1);
    if (id && id->pixels) { free(id->pixels); id->pixels = NULL; }
    return 0;
}

static int id_getWidth(lua_State* L) {
    ImageData* id = checkImageData(L, 1);
    lua_pushinteger(L, id->width);
    return 1;
}

static int id_getHeight(lua_State* L) {
    ImageData* id = checkImageData(L, 1);
    lua_pushinteger(L, id->height);
    return 1;
}

static int id_getDimensions(lua_State* L) {
    ImageData* id = checkImageData(L, 1);
    lua_pushinteger(L, id->width);
    lua_pushinteger(L, id->height);
    return 2;
}

static int id_getPixel(lua_State* L) {
    ImageData* id = checkImageData(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    if (x < 0 || y < 0 || x >= id->width || y >= id->height) {
        lua_pushnumber(L, 0.0);
        lua_pushnumber(L, 0.0);
        lua_pushnumber(L, 0.0);
        lua_pushnumber(L, 0.0);
        return 4;
    }
    const uint8_t* p = id->pixels + (y * id->width + x) * 4;
    lua_pushnumber(L, p[0] / 255.0);
    lua_pushnumber(L, p[1] / 255.0);
    lua_pushnumber(L, p[2] / 255.0);
    lua_pushnumber(L, p[3] / 255.0);
    return 4;
}

static inline uint8_t conv_channel(double v) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return (uint8_t)(v * 255.0);
}

static int id_setPixel(lua_State* L) {
    ImageData* id = checkImageData(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    if (x < 0 || y < 0 || x >= id->width || y >= id->height) return 0;
    double r = luaL_checknumber(L, 4);
    double g = luaL_checknumber(L, 5);
    double b = luaL_checknumber(L, 6);
    double a = luaL_optnumber(L, 7, 1.0);
    uint8_t* p = id->pixels + (y * id->width + x) * 4;
    p[0] = conv_channel(r);
    p[1] = conv_channel(g);
    p[2] = conv_channel(b);
    p[3] = conv_channel(a);
    return 0;
}

// mapPixel(fn): calls fn(x, y) for every pixel, expects r,g,b,a returns.
static int id_mapPixel(lua_State* L) {
    ImageData* id = checkImageData(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    for (int y = 0; y < id->height; y++) {
        for (int x = 0; x < id->width; x++) {
            lua_pushvalue(L, 2);
            lua_pushinteger(L, x);
            lua_pushinteger(L, y);
            if (lua_pcall(L, 2, 4, 0) != 0) {
                return lua_error(L);
            }
            double r = lua_tonumber(L, -4);
            double g = lua_tonumber(L, -3);
            double b = lua_tonumber(L, -2);
            double a = lua_tonumber(L, -1);
            uint8_t* p = id->pixels + (y * id->width + x) * 4;
            p[0] = conv_channel(r);
            p[1] = conv_channel(g);
            p[2] = conv_channel(b);
            p[3] = conv_channel(a);
            lua_pop(L, 4);
        }
    }
    return 0;
}

// ---- getString helper for the encoded FileData-like table -----------------

static int encode_getString(lua_State* L) {
    lua_getfield(L, 1, "__data");
    return 1;
}

// encode(format) -> { getString = function() ... end } with raw GR1T bytes.
static int id_encode(lua_State* L) {
    ImageData* id = checkImageData(L, 1);
    // format argument (e.g. "png") is ignored: the 3DS writes the raw GR1T
    // texture, not a PNG.  The file still gets a .png extension on disk
    // (texture_load_to_tex detects the magic, not the extension).
    size_t w = (size_t)id->width;
    size_t h = (size_t)id->height;
    size_t payload = w * h * 4;
    size_t total = 12 + payload;
    unsigned char* buf = (unsigned char*)malloc(total ? total : 1);
    if (!buf) return luaL_error(L, "Out of memory encoding image");

    buf[0] = 'G'; buf[1] = 'R'; buf[2] = '1'; buf[3] = 'T';
    buf[4] = (unsigned char)(w & 0xFF);
    buf[5] = (unsigned char)((w >> 8) & 0xFF);
    buf[6] = (unsigned char)((w >> 16) & 0xFF);
    buf[7] = (unsigned char)((w >> 24) & 0xFF);
    buf[8] = (unsigned char)(h & 0xFF);
    buf[9] = (unsigned char)((h >> 8) & 0xFF);
    buf[10] = (unsigned char)((h >> 16) & 0xFF);
    buf[11] = (unsigned char)((h >> 24) & 0xFF);
    memcpy(buf + 12, id->pixels, payload);

    lua_newtable(L);
    lua_pushlstring(L, (const char*)buf, total);
    lua_setfield(L, -2, "__data");
    lua_pushcfunction(L, encode_getString);
    lua_setfield(L, -2, "getString");
    free(buf);
    return 1;
}

// ---- ImageData metatable ---------------------------------------------------

static void create_imagedata_metatable(lua_State* L) {
    if (luaL_newmetatable(L, "ImageData")) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, id_gc);
        lua_setfield(L, -2, "__gc");

        lua_pushcfunction(L, id_getWidth);
        lua_setfield(L, -2, "getWidth");
        lua_pushcfunction(L, id_getHeight);
        lua_setfield(L, -2, "getHeight");
        lua_pushcfunction(L, id_getDimensions);
        lua_setfield(L, -2, "getDimensions");
        lua_pushcfunction(L, id_getPixel);
        lua_setfield(L, -2, "getPixel");
        lua_pushcfunction(L, id_setPixel);
        lua_setfield(L, -2, "setPixel");
        lua_pushcfunction(L, id_mapPixel);
        lua_setfield(L, -2, "mapPixel");
        lua_pushcfunction(L, id_encode);
        lua_setfield(L, -2, "encode");
    }
    lua_pop(L, 1); // pop metatable
}

// love.image._newImageData(width, height) -> ImageData userdata
static int l_newImageData(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return luaL_error(L, "Invalid ImageData size %dx%d", w, h);

    ImageData* id = (ImageData*)lua_newuserdata(L, sizeof(ImageData));
    id->width = w;
    id->height = h;
    id->pixels = (uint8_t*)calloc((size_t)w * h, 4);
    if (!id->pixels) return luaL_error(L, "Out of memory allocating ImageData");

    create_imagedata_metatable(L);
    // create_imagedata_metatable pops the metatable it pushes, so push it
    // again via luaL_getmetatable before lua_setmetatable consumes it.
    // (Without this the stack is [w, h, userdata] and lua_setmetatable(L,-2)
    //  treats the userdata as the metatable, pops it, and the function
    //  returns a number instead of the ImageData -> Lua state corruption.)
    luaL_getmetatable(L, "ImageData");
    lua_setmetatable(L, -2);
    return 1;
}

static const luaL_Reg image_funcs[] = {
    { "_newImageData", l_newImageData },
    { NULL, NULL }
};

extern "C" int luaopen_love_image(lua_State* L) {
    luaL_register(L, "love.image", image_funcs);
    // The engine's gate `love.image.newImageData` stays `false`: the 3DS
    // stub_script sets it right after this runs.  Do NOT set it here.
    return 1;
}
