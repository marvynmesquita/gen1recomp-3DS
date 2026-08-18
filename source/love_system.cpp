#include <3ds.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef __cplusplus
}
#endif

static int l_system_getOS(lua_State* L) {
    lua_pushstring(L, "3DS");
    return 1;
}

static const luaL_Reg system_funcs[] = {
    {"getOS", l_system_getOS},
    {NULL, NULL}
};

extern "C" int luaopen_love_system(lua_State* L) {
    luaL_register(L, "love.system", system_funcs);
    return 1;
}
