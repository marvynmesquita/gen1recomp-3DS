#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>

int luaopen_love_thread(lua_State* L);

#ifdef __cplusplus
}
#endif
