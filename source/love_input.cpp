// 3DS hid -> LOVE input bridge.
//
// The game (main.lua / Input.lua) is keyboard-event driven: it expects
// love.keypressed / love.keyreleased with LOVE key names ("up", "z", "x",
// "kpenter", "tab", ...) and a love.keyboard.isDown for held-state queries
// (Input:reconcile rebuilds holds from it after soft resets).  This module
// reads the 3DS buttons + circle pad each frame, synthesizes those events
// and dispatches them through the normal love.keypressed/released handlers,
// and exposes the minimal love.keyboard / love.event surfaces the game uses.
//
// Mapping (matches Input.lua DEFAULT_BINDINGS):
//   D-pad    -> arrow names  (up/down/left/right)
//   A        -> "z"  (GB A), B -> "x" (GB B)
//   START    -> "kpenter"     (GB start), SELECT -> "tab" (GB select)
//   circle pad -> WASD names  (w/s/a/d) so it rides the same bindings as the
//   D-pad but under a separate source key, avoiding a held D-pad direction
//   being cleared by a circle-pad release (and vice versa).
//   L/R (unused by the GB games) -> "l"/"r" so the launcher can watch for the
//   L+R+START combo that opens the in-game menu.

#include <3ds.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

extern "C" void sys_log(const char* format, ...);

static int g_wantQuit = 0;

extern "C" int love_input_want_quit(void) {
    return g_wantQuit;
}

// ---- touch screen (bottom screen launcher) --------------------------------
// hidTouchRead reports the bottom-screen touch even when nothing is touching
// (stale last position), so touch-down is keyed off KEY_TOUCH instead.

static touchPosition g_touch;
static bool g_touchDown = false;
static bool g_touchJustPressed = false;

static void pollTouch(void) {
    u32 held = hidKeysHeld();
    bool nowDown = (held & KEY_TOUCH) != 0;
    if (nowDown) hidTouchRead(&g_touch);
    g_touchJustPressed = nowDown && !g_touchDown;
    g_touchDown = nowDown;
}

extern "C" int love_input_touch_get_position(lua_State* L) {
    lua_pushinteger(L, g_touch.px);
    lua_pushinteger(L, g_touch.py);
    return 2;
}

extern "C" int love_input_touch_is_down(lua_State* L) {
    lua_pushboolean(L, g_touchDown);
    return 1;
}

extern "C" int love_input_touch_just_pressed(lua_State* L) {
    lua_pushboolean(L, g_touchJustPressed);
    return 1;
}

// ---- button -> love key map ------------------------------------------------

struct HidKey { u32 mask; const char* key; };
static const HidKey kKeys[] = {
    { KEY_DUP,    "up" },
    { KEY_DDOWN,  "down" },
    { KEY_DLEFT,  "left" },
    { KEY_DRIGHT, "right" },
    { KEY_A,      "z" },
    { KEY_B,      "x" },
    { KEY_START,  "kpenter" },
    { KEY_SELECT, "tab" },
    { KEY_L,      "l" },
    { KEY_R,      "r" },
};

// currently-held key set, for love.keyboard.isDown / Input:reconcile
static const char* g_down[16];
static int g_downCount = 0;

static void keyDown(const char* key) {
    for (int i = 0; i < g_downCount; i++) {
        if (strcmp(g_down[i], key) == 0) return;
    }
    if (g_downCount < 16) g_down[g_downCount++] = key;
}

static void keyUp(const char* key) {
    for (int i = 0; i < g_downCount; i++) {
        if (strcmp(g_down[i], key) == 0) {
            g_down[i] = g_down[g_downCount - 1];
            g_downCount--;
            return;
        }
    }
}

// ---- dispatch a synthesized key event through love.keypressed/released -----

static void dispatchKey(lua_State* L, int msgh, const char* fn, const char* key) {
    int top = lua_gettop(L);
    lua_getglobal(L, "love");
    lua_getfield(L, -1, fn);
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, key);        // key
        lua_pushstring(L, key);        // scancode
        int nargs = 2;
        if (strcmp(fn, "keypressed") == 0) {
            lua_pushboolean(L, 0);     // isrepeat
            nargs = 3;
        }
        if (lua_pcall(L, nargs, 0, msgh) != 0) {
            sys_log("Error calling love.%s: %s", fn, lua_tostring(L, -1));
            lua_pop(L, 1);             // pop error message
        }
    }
    lua_settop(L, top);
}

// ---- circle pad -> WASD digital directions (Input.lua hysteresis) ----------

static const float STICK_ON = 0.5f;
static const float STICK_OFF = 0.3f;
static int g_cpad[4] = { 0, 0, 0, 0 }; // up, down, left, right

static void updateCpad(lua_State* L, int msgh) {
    circlePosition cp;
    hidCircleRead(&cp);
    float x = (float)cp.dx / 32767.0f;
    float y = (float)cp.dy / 32767.0f;
    static const char* keys[4] = { "w", "s", "a", "d" };
    float mags[4] = { y, -y, -x, x };
    for (int i = 0; i < 4; i++) {
        if (!g_cpad[i] && mags[i] >= STICK_ON) {
            g_cpad[i] = 1;
            keyDown(keys[i]);
            dispatchKey(L, msgh, "keypressed", keys[i]);
        } else if (g_cpad[i] && mags[i] <= STICK_OFF) {
            g_cpad[i] = 0;
            keyUp(keys[i]);
            dispatchKey(L, msgh, "keyreleased", keys[i]);
        }
    }
}

// ---- per-frame poll, called from main.cpp before love.update --------------

extern "C" void love_input_poll(lua_State* L, int msgh) {
    hidScanInput();
    u32 down = hidKeysDown();
    u32 up = hidKeysUp();

    pollTouch();

    for (int i = 0; i < (int)(sizeof(kKeys) / sizeof(kKeys[0])); i++) {
        if (down & kKeys[i].mask) {
            keyDown(kKeys[i].key);
            dispatchKey(L, msgh, "keypressed", kKeys[i].key);
        } else if (up & kKeys[i].mask) {
            keyUp(kKeys[i].key);
            dispatchKey(L, msgh, "keyreleased", kKeys[i].key);
        }
    }

    updateCpad(L, msgh);
}

// ---- love.keyboard ---------------------------------------------------------

static int l_keyboard_isDown(lua_State* L) {
    int n = lua_gettop(L);
    int any = 0;
    for (int i = 1; i <= n && !any; i++) {
        const char* key = lua_tostring(L, i);
        if (!key) continue;
        for (int j = 0; j < g_downCount; j++) {
            if (strcmp(g_down[j], key) == 0) { any = 1; break; }
        }
    }
    lua_pushboolean(L, any);
    return 1;
}

static int l_keyboard_setKeyRepeat(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

static int l_keyboard_setTextInput(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

static int l_keyboard_hasTextInput(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

static int l_keyboard_hasKeyRepeat(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

static int l_keyboard_getKeyFromScancode(lua_State* L) {
    lua_pushvalue(L, 1);
    return 1;
}

static int l_keyboard_getScancodeFromKey(lua_State* L) {
    lua_pushvalue(L, 1);
    return 1;
}

static const luaL_Reg keyboard_funcs[] = {
    { "isDown", l_keyboard_isDown },
    { "setKeyRepeat", l_keyboard_setKeyRepeat },
    { "setTextInput", l_keyboard_setTextInput },
    { "hasTextInput", l_keyboard_hasTextInput },
    { "hasKeyRepeat", l_keyboard_hasKeyRepeat },
    { "getKeyFromScancode", l_keyboard_getKeyFromScancode },
    { "getScancodeFromKey", l_keyboard_getScancodeFromKey },
    { NULL, NULL }
};

extern "C" int luaopen_love_keyboard(lua_State* L) {
    lua_getglobal(L, "love");
    lua_newtable(L);
    luaL_register(L, NULL, keyboard_funcs);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "keyboard");
    lua_remove(L, -2); // remove 'love' table
    return 1;
}

// ---- love.event ------------------------------------------------------------

static int l_event_quit(lua_State* L) {
    g_wantQuit = 1;
    return 0;
}

static int l_event_pump(lua_State* L) {
    return 0;
}

static int l_event_iter(lua_State* L) {
    return 0; // no queued events
}

static int l_event_poll(lua_State* L) {
    lua_pushcfunction(L, l_event_iter);
    lua_pushnil(L);
    lua_pushnil(L);
    return 3;
}

static int l_event_push(lua_State* L) {
    return 0;
}

static int l_event_clear(lua_State* L) {
    return 0;
}

static const luaL_Reg event_funcs[] = {
    { "quit", l_event_quit },
    { "pump", l_event_pump },
    { "poll", l_event_poll },
    { "push", l_event_push },
    { "clear", l_event_clear },
    { NULL, NULL }
};

extern "C" int luaopen_love_event(lua_State* L) {
    lua_getglobal(L, "love");
    lua_newtable(L);
    luaL_register(L, NULL, event_funcs);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "event");
    lua_remove(L, -2); // remove 'love' table
    return 1;
}
