#include "love_thread.h"
#include <3ds.h>
#include <string>
#include <queue>
#include <map>
#include <stdlib.h>

extern "C" void sys_log(const char* format, ...);

#ifdef __cplusplus
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
extern int luaopen_love_sound(lua_State* L);
extern int luaopen_love_synth(lua_State* L);
}
#endif

struct SoundData {
    s16* data;
    int samples;
    int sampleRate;
    int bitDepth;
    int channels;
};

enum MsgType {
    MSG_STRING,
    MSG_SOUNDDATA,
    MSG_BOOLEAN
};

struct Message {
    MsgType type;
    std::string str;
    s16* sd_data;
    int sd_samples;
    int sd_rate;
    int sd_depth;
    int sd_channels;
    bool b;
};

struct Channel {
    LightLock lock;
    std::queue<Message> q;
};

static std::map<std::string, Channel*> channels;
static LightLock channels_lock;

static Channel* get_channel(const std::string& name) {
    LightLock_Lock(&channels_lock);
    auto it = channels.find(name);
    if (it != channels.end()) {
        LightLock_Unlock(&channels_lock);
        return it->second;
    }
    Channel* c = new Channel();
    LightLock_Init(&c->lock);
    channels[name] = c;
    LightLock_Unlock(&channels_lock);
    return c;
}

static int l_channel_push(lua_State* L) {
    Channel* c = *(Channel**)luaL_checkudata(L, 1, "Channel");
    Message m;
    if (lua_type(L, 2) == LUA_TSTRING) {
        m.type = MSG_STRING;
        m.str = lua_tostring(L, 2);
    } else if (lua_type(L, 2) == LUA_TBOOLEAN) {
        m.type = MSG_BOOLEAN;
        m.b = lua_toboolean(L, 2);
    } else if (lua_type(L, 2) == LUA_TUSERDATA) {
        SoundData* sd = (SoundData*)lua_touserdata(L, 2);
        m.type = MSG_SOUNDDATA;
        m.sd_data = sd->data;
        m.sd_samples = sd->samples;
        m.sd_rate = sd->sampleRate;
        m.sd_depth = sd->bitDepth;
        m.sd_channels = sd->channels;
        sd->data = NULL; // Take ownership
    } else {
        return luaL_error(L, "Unsupported type for Channel:push");
    }
    
    LightLock_Lock(&c->lock);
    c->q.push(m);
    LightLock_Unlock(&c->lock);
    return 0;
}

static int l_channel_pop(lua_State* L) {
    Channel* c = *(Channel**)luaL_checkudata(L, 1, "Channel");
    LightLock_Lock(&c->lock);
    if (c->q.empty()) {
        LightLock_Unlock(&c->lock);
        lua_pushnil(L);
        return 1;
    }
    Message m = c->q.front();
    c->q.pop();
    LightLock_Unlock(&c->lock);
    
    if (m.type == MSG_STRING) {
        lua_pushstring(L, m.str.c_str());
    } else if (m.type == MSG_BOOLEAN) {
        lua_pushboolean(L, m.b);
    } else if (m.type == MSG_SOUNDDATA) {
        SoundData* sd = (SoundData*)lua_newuserdata(L, sizeof(SoundData));
        sd->data = m.sd_data;
        sd->samples = m.sd_samples;
        sd->sampleRate = m.sd_rate;
        sd->bitDepth = m.sd_depth;
        sd->channels = m.sd_channels;
        luaL_getmetatable(L, "SoundData");
        lua_setmetatable(L, -2);
    }
    return 1;
}

static int l_channel_clear(lua_State* L) {
    Channel* c = *(Channel**)luaL_checkudata(L, 1, "Channel");
    LightLock_Lock(&c->lock);
    while(!c->q.empty()) {
        Message m = c->q.front();
        if (m.type == MSG_SOUNDDATA && m.sd_data) {
            free(m.sd_data);
        }
        c->q.pop();
    }
    LightLock_Unlock(&c->lock);
    return 0;
}

static int l_channel_getCount(lua_State* L) {
    Channel* c = *(Channel**)luaL_checkudata(L, 1, "Channel");
    LightLock_Lock(&c->lock);
    int count = c->q.size();
    LightLock_Unlock(&c->lock);
    lua_pushinteger(L, count);
    return 1;
}

static const luaL_Reg channel_methods[] = {
    {"push", l_channel_push},
    {"pop", l_channel_pop},
    {"clear", l_channel_clear},
    {"getCount", l_channel_getCount},
    {NULL, NULL}
};

static int l_thread_getChannel(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    Channel* c = get_channel(name);
    Channel** ptr = (Channel**)lua_newuserdata(L, sizeof(Channel*));
    *ptr = c;
    luaL_getmetatable(L, "Channel");
    lua_setmetatable(L, -2);
    return 1;
}

struct ThreadObj {
    Thread thread;
    std::string script;
};

extern "C" {
    int luaopen_bit(lua_State *L);
}

static void thread_entry(void* arg) {
    ThreadObj* t = (ThreadObj*)arg;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // A fresh worker Lua state starts empty: create the "love" global table
    // before the stub script and the module openers below index it.  Without
    // it luaopen_love_thread's lua_setfield on the nil global raises an
    // unprotected Lua error, which panics and exits the whole process the
    // moment the first thread is started.
    lua_newtable(L);
    lua_setglobal(L, "love");

    // Register LuaBitOp in package.preload: ChipSynth.lua does require("bit")
    // when the worker loads it, and this state has no preload entry for it.
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_bit);
    lua_setfield(L, -2, "bit");
    lua_pop(L, 2);

    const char* stub_script = R"LUA_STUB(
local stubs = {'audio', 'data', 'filesystem', 'image', 'joystick', 'keyboard', 'math', 'mouse', 'sound', 'touch', 'window', 'event', 'graphics', 'system'}
for _, name in ipairs(stubs) do
  love[name] = love[name] or {}
  setmetatable(love[name], {__index = function(t, k)
    return function() end
  end})
end
love.math = love.math or {}
love.math.random = math.random
love.filesystem.getInfo = function(path)
  local f = io.open('romfs:/' .. path, 'rb')
  if f then f:close() return {} else return nil end
end
love.filesystem.getRealDirectory = function(path) return 'romfs' end
love.filesystem.getUserDirectory = function() return 'romfs' end
love.filesystem.getSaveDirectory = function() return 'romfs' end
love.filesystem.getAppdataDirectory = function() return 'romfs' end
love.filesystem.read = function(path)
  local f = io.open('romfs:/' .. path, 'rb')
  if not f then return nil, 'file not found' end
  local d = f:read('*a') f:close() return d
end
love.filesystem.load = function(path) return loadfile('romfs:/' .. path) end
)LUA_STUB";

    if (luaL_dostring(L, stub_script) != 0) {
        sys_log("Worker stub script error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    luaopen_love_thread(L);
    luaopen_love_sound(L);
    luaopen_love_synth(L);
    lua_pop(L, 1); // pop love.synth table
    lua_pop(L, 1); // pop the love.sound module table left by luaopen_love_sound
    
    // Register love.timer.sleep as real svcSleepThread so chip_worker.lua yields properly
    lua_getglobal(L, "love");
    lua_getfield(L, -1, "timer");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, -2, "timer");
        lua_getfield(L, -1, "timer");
    }
    lua_pushcfunction(L, [](lua_State* L2) -> int {
        double secs = luaL_optnumber(L2, 1, 0.0);
        if (secs > 0) {
            svcSleepThread((s64)(secs * 1000000000.0));
        }
        return 0;
    });
    lua_setfield(L, -2, "sleep");
    lua_pushcfunction(L, [](lua_State* L2) -> int {
        lua_pushnumber(L2, (double)svcGetSystemTick() / 268123480.0);
        return 1;
    });
    lua_setfield(L, -2, "getTime");
    lua_pop(L, 2); // pop timer and love

    // Expose the C sys_log (timestamped, goes to the game log) to the worker's
    // Lua state so chip_worker.lua can report its progress at the game log.
    lua_pushcfunction(L, [](lua_State* L2) -> int {
        sys_log("%s", luaL_optstring(L2, 1, ""));
        return 0;
    });
    lua_setglobal(L, "syslog");

    // Worker scripts call require("love.thread"), require("love.timer"), etc.
    // and expect the love.<name> tables back.  Register them into
    // package.loaded so the default searcher resolves them.
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    static const char* love_modules[] = {
        "thread", "timer", "sound", "filesystem", "data", "system"
    };
    for (size_t i = 0; i < sizeof(love_modules) / sizeof(love_modules[0]); i++) {
        lua_getglobal(L, "love");
        lua_getfield(L, -1, love_modules[i]);
        if (lua_istable(L, -1)) {
            std::string key = "love.";
            key += love_modules[i];
            lua_setfield(L, -3, key.c_str());
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // pop love
    }
    lua_pop(L, 2); // pop loaded and package
    
    std::string path = "romfs:/" + t->script;
    if (luaL_dofile(L, path.c_str()) != 0) {
        printf("Worker Thread Error: %s\n", lua_tostring(L, -1));
        sys_log("Worker Thread Error: %s", lua_tostring(L, -1));
    }
    
    lua_close(L);
}

static int l_thread_newThread(lua_State* L) {
    const char* script = luaL_checkstring(L, 1);
    ThreadObj* t = new ThreadObj();
    t->script = script;
    t->thread = NULL;
    
    ThreadObj** ptr = (ThreadObj**)lua_newuserdata(L, sizeof(ThreadObj*));
    *ptr = t;
    luaL_getmetatable(L, "Thread");
    lua_setmetatable(L, -2);
    return 1;
}

static int l_thread_start(lua_State* L) {
    ThreadObj* t = *(ThreadObj**)luaL_checkudata(L, 1, "Thread");
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Run the worker BELOW the app thread.  Synthesis is audio-latency-tolerant
    // (the deep playback queue absorbs it), but a higher-priority worker
    // preempts the render thread while it loads the synth / fills the queue,
    // which is exactly the frame hit on the intro.  Lower priority keeps the
    // renderer in charge and lets the worker synthesize during the frame's
    // VSync waits.  Affinity -1 lets the scheduler place it on core 1, which
    // APT_SetAppCpuTimeLimit (main.cpp) shares with the system -- that is what
    // gives the worker a real slice of CPU instead of starving on core 0 next
    // to the continuously-rendering main thread.
    if (prio > 0x3E) prio = 0x3E;
    t->thread = threadCreate(thread_entry, t, 128 * 1024, prio + 1, 1, false);
    return 0;
}

static int l_thread_wait(lua_State* L) {
    ThreadObj* t = *(ThreadObj**)luaL_checkudata(L, 1, "Thread");
    if (t->thread) {
        threadJoin(t->thread, U64_MAX);
        threadFree(t->thread);
        t->thread = NULL;
    }
    return 0;
}

static int l_thread_getError(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

static const luaL_Reg thread_methods[] = {
    {"start", l_thread_start},
    {"wait", l_thread_wait},
    {"getError", l_thread_getError},
    {NULL, NULL}
};

static const luaL_Reg thread_funcs[] = {
    {"getChannel", l_thread_getChannel},
    {"newThread", l_thread_newThread},
    {NULL, NULL}
};

extern "C" int luaopen_love_thread(lua_State* L) {
    static bool initialized = false;
    if (!initialized) {
        LightLock_Init(&channels_lock);
        initialized = true;
    }

    luaL_newmetatable(L, "Channel");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, channel_methods);
    lua_pop(L, 1);
    
    luaL_newmetatable(L, "Thread");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, thread_methods);
    lua_pop(L, 1);

    lua_getglobal(L, "love");
    lua_newtable(L);
    luaL_register(L, NULL, thread_funcs);
    lua_setfield(L, -2, "thread");
    lua_pop(L, 1);
    
    return 0;
}
