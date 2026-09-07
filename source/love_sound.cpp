#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

struct SoundData {
    s16* data;
    int samples;
    int sampleRate;
    int bitDepth;
    int channels;
};

static int l_sounddata_gc(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    if (sd->data) {
        free(sd->data);
        sd->data = NULL;
    }
    return 0;
}

static int l_sounddata_setSample(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    int index = luaL_checkinteger(L, 2); // 0-indexed in love
    int channel = luaL_checkinteger(L, 3); // 1-indexed in love
    float value = luaL_checknumber(L, 4);

    if (index < 0 || index >= sd->samples) return 0;
    if (channel < 1 || channel > sd->channels) return 0;

    // Convert float [-1.0, 1.0] to s16 [-32768, 32767]
    s16 pcm_val;
    if (value >= 1.0f) pcm_val = 32767;
    else if (value <= -1.0f) pcm_val = -32768;
    else pcm_val = (s16)(value * 32767.0f);

    sd->data[index * sd->channels + (channel - 1)] = pcm_val;
    return 0;
}

static int l_sounddata_getSample(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    if (!sd->data) {
        // Backing PCM buffer was freed or never allocated; reading a sample
        // from it would dereference NULL.  Report silence.
        lua_pushnumber(L, 0.0f);
        return 1;
    }
    int index = luaL_checkinteger(L, 2);
    int channel = luaL_checkinteger(L, 3);

    if (index < 0 || index >= sd->samples || channel < 1 || channel > sd->channels) {
        lua_pushnumber(L, 0.0);
        return 1;
    }

    s16 pcm_val = sd->data[index * sd->channels + (channel - 1)];
    lua_pushnumber(L, (float)pcm_val / 32768.0f);
    return 1;
}

static int l_sounddata_getSampleCount(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    lua_pushinteger(L, sd->samples);
    return 1;
}

static int l_sounddata_getChannelCount(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    lua_pushinteger(L, sd->channels);
    return 1;
}

static int l_sounddata_getSampleRate(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    lua_pushinteger(L, sd->sampleRate);
    return 1;
}

static int l_sounddata_getDataValid(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    lua_pushboolean(L, sd->data != NULL);
    return 1;
}

static const luaL_Reg sounddata_methods[] = {
    {"setSample", l_sounddata_setSample},
    {"getSample", l_sounddata_getSample},
    {"getSampleCount", l_sounddata_getSampleCount},
    {"getChannelCount", l_sounddata_getChannelCount},
    {"getSampleRate", l_sounddata_getSampleRate},
    {"getDataValid", l_sounddata_getDataValid},
    {"__gc", l_sounddata_gc},
    {NULL, NULL}
};

static int l_sound_newSoundData(lua_State* L) {
    int samples = luaL_checkinteger(L, 1);
    int rate = luaL_optinteger(L, 2, 44100);
    int bits = luaL_optinteger(L, 3, 16);
    int channels = luaL_optinteger(L, 4, 1); // 1 = mono, 2 = stereo

    SoundData* sd = (SoundData*)lua_newuserdata(L, sizeof(SoundData));
    sd->samples = samples;
    sd->sampleRate = rate;
    sd->bitDepth = bits;
    sd->channels = channels;
    sd->data = (s16*)malloc(samples * channels * sizeof(s16));
    if (sd->data) {
        memset(sd->data, 0, samples * channels * sizeof(s16));
    }

    luaL_getmetatable(L, "SoundData");
    lua_setmetatable(L, -2);
    return 1;
}

// Explicitly release the PCM buffer of a SoundData.  On the 3DS the Lua GC is
// stopped during gameplay, so any SoundData whose buffer is no longer needed
// (e.g. stale music buffers dropped by the streaming thread) must be freed
// deterministically to keep the heap from growing unbounded.
static int l_sound_freeSoundData(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    if (sd->data) {
        free(sd->data);
        sd->data = NULL;
    }
    return 0;
}

static const luaL_Reg sound_funcs[] = {
    {"newSoundData", l_sound_newSoundData},
    {"freeSoundData", l_sound_freeSoundData},
    {NULL, NULL}
};

extern "C" int luaopen_love_sound(lua_State* L) {
    luaL_newmetatable(L, "SoundData");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, sounddata_methods);
    lua_pop(L, 1);

    luaL_register(L, "love.sound", sound_funcs);
    return 1;
}
