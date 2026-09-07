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

extern "C" void sys_log(const char* format, ...);

// Track whether ndspInit() succeeded.  When it fails (e.g. D880A7FA on some
// 3DS builds) the DSP channels are never properly initialized, so
// ndspChnSetPaused / ndspChnIsPaused operate on garbage state and
// isPlaying() incorrectly returns true for ever.  Expose this flag so the
// Lua waitSound check can skip the (broken) sound and let the battle queue
// advance.
static bool g_ndspReady = false;

extern "C" void love_audio_setNdspReady(bool ready) {
    g_ndspReady = ready;
}

// We need the layout of SoundData to read it directly
struct SoundData {
    s16* data;
    int samples;
    int sampleRate;
    int bitDepth;
    int channels;
};

static int g_nextChannel = 0;

struct Source {
    int channel;
    int sampleRate;
    int channels;
    int bufferCount;
    ndspWaveBuf* waveBufs;
    s16** linearMem;
    size_t linearMemSize;
    bool isStatic;
};

static int l_source_gc(lua_State* L) {
    Source* src = (Source*)luaL_checkudata(L, 1, "Source");
    ndspChnWaveBufClear(src->channel);
    if (src->waveBufs) {
        free(src->waveBufs);
        src->waveBufs = NULL;
    }
    if (src->linearMem) {
        for (int i = 0; i < src->bufferCount; i++) {
            if (src->linearMem[i]) {
                linearFree(src->linearMem[i]);
            }
        }
        free(src->linearMem);
        src->linearMem = NULL;
    }
    return 0;
}

static int l_source_play(lua_State* L) {
    Source* src = (Source*)luaL_checkudata(L, 1, "Source");
    if (src->isStatic) {
        ndspChnWaveBufClear(src->channel);
        ndspChnWaveBufAdd(src->channel, &src->waveBufs[0]);
    }
    ndspChnSetPaused(src->channel, false);
    return 0;
}

static int l_source_stop(lua_State* L) {
    Source* src = (Source*)luaL_checkudata(L, 1, "Source");
    ndspChnWaveBufClear(src->channel);
    return 0;
}

static int l_source_setVolume(lua_State* L) {
    Source* src = (Source*)luaL_checkudata(L, 1, "Source");
    float vol = luaL_checknumber(L, 2);
    // vol is typically 0.0 to 1.0, though can be higher. NDSP mix is up to 1.0.
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = vol; // left
    mix[1] = vol; // right
    ndspChnSetMix(src->channel, mix);
    return 0;
}

static int l_source_queue(lua_State* L) {
    Source* src = (Source*)luaL_checkudata(L, 1, "Source");
    if (src->isStatic) {
        sys_log("queue error: Cannot queue on static Source");
        return luaL_error(L, "Cannot queue on static Source.");
    }
    
    SoundData* sd = (SoundData*)luaL_checkudata(L, 2, "SoundData");
    // Guard: if the backing PCM buffer was already released (either by a
    // previous queue on this shared userdata or by freeSoundData from another
    // thread), memcpy from NULL would hard-crash.  The samples were already
    // copied into the source's linear buffer by that earlier queue, so this
    // call is a no-op: report success so the caller RELEASES the buffer
    // instead of holding it and letting ~43 buffers/second accumulate in the
    // main thread's stopped-GC Lua heap (LUA_ERRMEM after a few minutes).
    if (!sd->data) {
        lua_pushboolean(L, 1);
        return 1;
    }
    
    // Find free buffer
    for (int i = 0; i < src->bufferCount; i++) {
        if (src->waveBufs[i].status == NDSP_WBUF_DONE || src->waveBufs[i].status == NDSP_WBUF_FREE) {
            size_t size = sd->samples * sd->channels * sizeof(s16);
            if (size > src->linearMemSize) {
                sys_log("queue error: SoundData exceeds QueueableSource buffer size. %d > %d", (int)size, (int)src->linearMemSize);
                return luaL_error(L, "SoundData exceeds QueueableSource buffer size.");
            }
            
            memcpy(src->linearMem[i], sd->data, size);
            DSP_FlushDataCache(src->linearMem[i], size);

            // The samples were copied into the source's own linear buffer, so
            // the SoundData's malloc'd heap is no longer needed.  Free it right
            // away instead of relying on the Lua GC: on the 3DS the collector
            // is stopped during gameplay (collectgarbage("stop")), so deferring
            // this to __gc let ~43 music buffers/second (~88KB/s) pile up in
            // the main-thread heap and eventually trip LUA_ERRMEM
            // ("not enough memory") mid-battle/map.  Deterministic release
            // keeps the queue at a constant memory footprint.
            free(sd->data);
            sd->data = NULL;

            memset(&src->waveBufs[i], 0, sizeof(ndspWaveBuf));
            src->waveBufs[i].data_vaddr = src->linearMem[i];
            src->waveBufs[i].nsamples = sd->samples;
            src->waveBufs[i].looping = false;
            src->waveBufs[i].status = NDSP_WBUF_FREE;
            
            DSP_FlushDataCache(&src->waveBufs[i], sizeof(ndspWaveBuf));
            
            // Check if buffer is silent
            s16 max_val = 0;
            s16* ptr = src->linearMem[i];
            for (size_t k = 0; k < size / 2; k++) {
                if (ptr[k] != 0) {
                    s16 abs_val = ptr[k] > 0 ? ptr[k] : -ptr[k];
                    if (abs_val > max_val) max_val = abs_val;
                }
            }
            
            ndspChnWaveBufAdd(src->channel, &src->waveBufs[i]);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    
    lua_pushboolean(L, 0); // Queue full
    return 1;
}

static int l_source_getFreeBufferCount(lua_State* L) {
    Source* src = (Source*)luaL_checkudata(L, 1, "Source");
    if (src->isStatic) {
        lua_pushinteger(L, 0);
        return 1;
    }
    
    int freeCount = 0;
    for (int i = 0; i < src->bufferCount; i++) {
        if (src->waveBufs[i].status == NDSP_WBUF_DONE || src->waveBufs[i].status == NDSP_WBUF_FREE) {
            freeCount++;
        }
    }
    lua_pushinteger(L, freeCount);
    return 1;
}

static int l_source_isPlaying(lua_State* L) {
    Source* src = (Source*)luaL_checkudata(L, 1, "Source");
    // When ndspInit failed the DSP channels are never initialized, so
    // ndspChnIsPaused returns garbage.  Report "not playing" so the Lua
    // battle-queue waitSound check can advance and the game doesn't freeze
    // waiting on a sound that can never finish.
    if (!g_ndspReady) { lua_pushboolean(L, 0); return 1; }
    bool playing = false;
    if (src->isStatic) {
        // For static sources, check if the channel is playing and not paused.
        // In citro3d, we don't have a direct "is playing" flag for static wavebufs that completed, 
        // but status becomes NDSP_WBUF_DONE.
        if (src->waveBufs[0].status != NDSP_WBUF_DONE && src->waveBufs[0].status != NDSP_WBUF_FREE) {
            playing = !ndspChnIsPaused(src->channel);
        }
    } else {
        // For streaming sources, if there's any buffer queued and not paused, it's playing
        playing = !ndspChnIsPaused(src->channel);
        if (playing) {
            int queued = 0;
            for (int i = 0; i < src->bufferCount; i++) {
                if (src->waveBufs[i].status == NDSP_WBUF_QUEUED || src->waveBufs[i].status == NDSP_WBUF_PLAYING) {
                    queued++;
                }
            }
            if (queued == 0) playing = false;
        }
    }
    lua_pushboolean(L, playing);
    return 1;
}

static int l_source_setLooping(lua_State* L) {
    Source* src = (Source*)luaL_checkudata(L, 1, "Source");
    bool looping = lua_toboolean(L, 2);
    if (src->isStatic) {
        src->waveBufs[0].looping = looping;
        DSP_FlushDataCache(&src->waveBufs[0], sizeof(ndspWaveBuf));
    }
    // Queueable sources loop inherently by queueing buffers in love2d, 
    // or we just ignore looping flag for queueable sources.
    return 0;
}

static const luaL_Reg source_methods[] = {
    {"play", l_source_play},
    {"stop", l_source_stop},
    {"setVolume", l_source_setVolume},
    {"queue", l_source_queue},
    {"getFreeBufferCount", l_source_getFreeBufferCount},
    {"isPlaying", l_source_isPlaying},
    {"setLooping", l_source_setLooping},
    {"__gc", l_source_gc},
    {NULL, NULL}
};

static int l_audio_newQueueableSource(lua_State* L) {
    int sampleRate = luaL_checkinteger(L, 1);
    int channels = luaL_checkinteger(L, 3);
    int bufferCount = luaL_optinteger(L, 4, 8);
    
    // Per-buffer slot size.  ChipSynth streams 512-sample stereo buffers
    // (2048 bytes); size each slot with headroom (supports ~2048 stereo
    // samples) so the whole source stays a small fraction of the linear heap.
    // The old 65536-byte slots allocated and zeroed 4MB per song change on the
    // render thread, which hitched the frame and could exhaust the heap.
    size_t linearMemSize = 8192; 

    Source* src = (Source*)lua_newuserdata(L, sizeof(Source));
    src->channel = g_nextChannel++;
    if (g_nextChannel >= 24) g_nextChannel = 0; // Wrap around if we run out of channels
    
    src->sampleRate = sampleRate;
    src->channels = channels;
    src->bufferCount = bufferCount;
    src->linearMemSize = linearMemSize;
    src->isStatic = false;
    
    src->waveBufs = (ndspWaveBuf*)calloc(bufferCount, sizeof(ndspWaveBuf));
    src->linearMem = (s16**)malloc(bufferCount * sizeof(s16*));
    for (int i = 0; i < bufferCount; i++) {
        src->linearMem[i] = (s16*)linearAlloc(linearMemSize);
        memset(src->linearMem[i], 0, linearMemSize);
    }
    
    ndspChnReset(src->channel);
    ndspChnSetInterp(src->channel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(src->channel, sampleRate);
    ndspChnSetFormat(src->channel, channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0;
    mix[1] = 1.0;
    ndspChnSetMix(src->channel, mix);
    
    luaL_getmetatable(L, "Source");
    lua_setmetatable(L, -2);
    return 1;
}

static int l_audio_newSource(lua_State* L) {
    SoundData* sd = (SoundData*)luaL_checkudata(L, 1, "SoundData");
    (void)luaL_optstring(L, 2, "static");
    
    Source* src = (Source*)lua_newuserdata(L, sizeof(Source));
    src->channel = g_nextChannel++;
    if (g_nextChannel >= 24) g_nextChannel = 0;
    
    src->sampleRate = sd->sampleRate;
    src->channels = sd->channels;
    src->bufferCount = 1;
    src->isStatic = true;
    
    size_t size = sd->samples * sd->channels * sizeof(s16);
    src->linearMemSize = size;
    
    src->waveBufs = (ndspWaveBuf*)calloc(1, sizeof(ndspWaveBuf));
    src->linearMem = (s16**)malloc(1 * sizeof(s16*));
    src->linearMem[0] = (s16*)linearAlloc(size);
    memcpy(src->linearMem[0], sd->data, size);
    DSP_FlushDataCache(src->linearMem[0], size);
    
    src->waveBufs[0].data_vaddr = src->linearMem[0];
    src->waveBufs[0].nsamples = sd->samples;
    src->waveBufs[0].looping = false; // static sources don't loop by default in love, unless setLooping is called
    src->waveBufs[0].status = NDSP_WBUF_FREE;
    
    DSP_FlushDataCache(&src->waveBufs[0], sizeof(ndspWaveBuf));
    
    ndspChnReset(src->channel);
    ndspChnSetInterp(src->channel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(src->channel, src->sampleRate);
    ndspChnSetFormat(src->channel, src->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0;
    mix[1] = 1.0;
    ndspChnSetMix(src->channel, mix);
    
    luaL_getmetatable(L, "Source");
    lua_setmetatable(L, -2);
    return 1;
}

static const luaL_Reg audio_funcs[] = {
    {"newQueueableSource", l_audio_newQueueableSource},
    {"newSource", l_audio_newSource},
    {NULL, NULL}
};

extern "C" int luaopen_love_audio(lua_State* L) {
    luaL_newmetatable(L, "Source");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, source_methods);
    lua_pop(L, 1);

    luaL_register(L, "love.audio", audio_funcs);
    return 1;
}
