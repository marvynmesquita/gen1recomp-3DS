// C synthesis core for the Game Boy audio synth (romfs/src/core/ChipSynth.lua).
//
// The Lua ChipSynth renders one PCM sample as ~150 Lua bytecode ops (event
// state, envelope, sweep/slide/vibrato, noise LFSR, duty/wave lookup, mixer,
// HPF/LPF) on top of a slow interpreter, so realtime streaming needs ~22050
// samples/s and the platform can only produce ~6-7k/s.  This module moves the
// per-sample hot path to C while keeping ALL event parsing in Lua: Lua decodes
// the music bytecode into events, hands each event's compact state to C via
// setEvent, and C renders samples until a channel hits its event boundary,
// then Lua advances the channel and feeds the next event.
//
// The API mirrors the Lua synth's rendering contract exactly:
//   love.synth.new(rate, {hw1, hw2, ...})      create a synth engine
//   synth:setChannelVolumes({1,1,1,1})
//   synth:setChannelPitches({1,1,1,1})
//   synth:setEvent(ch, event, tailDrum, tailSample, waveTable)  push current event state
//   synth:resetNoise(ch)
//   synth:eventSample(ch)                     within-event counter (boundary)
//   synth:render(sd, offset, count, stereo)   render frames; returns rendered

#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

#define SYNTH_MAX_CHANNELS 8

struct SoundData {
    s16* data;
    int samples;
    int sampleRate;
    int bitDepth;
    int channels;
};

// Duty patterns match WAVE_PATTERN_TABLES in ChipSynth.lua (index 0-3).
static const int DUTY[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
};

// NOISE_DIVISORS in ChipSynth.lua (index 0-7).
static const float NOISE_DIVISORS[8] = {
    8, 16, 32, 48, 64, 80, 96, 112,
};

static const int GB_CLOCK = 4194304;
static const float LPF_ALPHA = 0.8f;
static const float MIX_SCALE = 0.5f;

// --- small field readers for the event table (used only at event boundaries) ---

static int luaL_getfield_int_or_default(lua_State* L, int tableIndex,
                                        const char* key, int def) {
    lua_getfield(L, tableIndex, key);
    int v = def;
    if (!lua_isnil(L, -1)) v = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}

static int luaL_getfield_bool(lua_State* L, int tableIndex,
                              const char* key, int def) {
    lua_getfield(L, tableIndex, key);
    int v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

// pan: nil/missing = both sides (1), false = mute that side, true = on
static int luaL_getfield_pan(lua_State* L, int tableIndex, const char* key) {
    lua_getfield(L, tableIndex, key);
    int v = 1;
    if (lua_isboolean(L, -1)) v = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    return v;
}

static float luaL_getfield_num_or_default(lua_State* L, int tableIndex,
                                          const char* key, float def) {
    lua_getfield(L, tableIndex, key);
    float v = def;
    if (!lua_isnil(L, -1)) v = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

typedef struct {
    int segIndex;         // 1-based segment index in the drum table
    int segValid;
    int segStart, segEnd;
    int segVolume, segFade, segParameter;
    int drumRef;          // lua registry ref to the drum segment table
} DrumState;

typedef struct {
    int hw;               // 1-4
    // current event
    int active;
    int ended;
    int silence, drum, noise, wave;
    int duty;             // 0-3
    int hasDutyPattern;
    int dutyPattern[4];
    int reg;              // event.register
    float volume;
    int fade;
    int hasSweep, sweepPace, sweepSubtract, sweepShift;
    int hasSlide;
    float slideTarget;
    int slideFrames;
    int hasVibrato, vibratoDelay, vibratoRate, vibratoAbove, vibratoBelow;
    int noiseParameter;
    int waveInstrument;
    float waveLevel;
    float waveTable[32];
    int waveValid;
    int eventSample;
    int eventSamples;
    float phase;
    int panLeft;
    int panRight;
    unsigned int noiseLfsr;
    float noiseClock;
    // tail (a previous drum ringing through silence)
    int hasTail;
    int tailSample;
    int tailEnd;
    int tailRef;
    DrumState tail;
    DrumState cur;
} ChanState;

typedef struct {
    int rate;
    int nch;
    ChanState* channels;
    float vol[4];
    float pitch[4];
    float hpfCap[2];
    float lpf[2];
    float hpfCharge;
} Synth;

static void reset_noise(ChanState* ch) {
    ch->noiseLfsr = 0x7FFF;
    ch->noiseClock = 0;
}

static void clock_noise(ChanState* ch, int width7) {
    unsigned int fb = ((ch->noiseLfsr & 1) ^ ((ch->noiseLfsr >> 1) & 1));
    ch->noiseLfsr = (ch->noiseLfsr >> 1) | (fb << 14);
    if (width7) {
        ch->noiseLfsr = (ch->noiseLfsr & ~0x40u) | (fb << 6);
    }
}

// Read segment segIndex (1-based) of the drum table held at registry ref.
static int read_segment(lua_State* L, int ref, int segIndex,
                        int* start, int* end, int* vol, int* fade, int* param) {
    lua_pushinteger(L, ref); lua_gettable(L, LUA_REGISTRYINDEX);
    lua_pushinteger(L, segIndex); lua_gettable(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return 0;
    }
    lua_getfield(L, -1, "startSample"); *start = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "endSample");   *end   = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "volume");      *vol   = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "fade");        *fade  = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, -1, "parameter");   *param = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_pop(L, 2);
    return 1;
}

static float envelope(float volume, int fade, float elapsed) {
    if (fade == 0) return volume;
    int steps = (int)floorf(elapsed / (fabsf((float)fade) / 64.0f));
    if (fade > 0) {
        float v = volume - steps;
        return v < 0 ? 0 : v;
    }
    float v = volume + steps;
    return v > 15 ? 15 : v;
}

static int swept_register(ChanState* ch, int reg, float elapsed) {
    if (ch->sweepShift == 0) return reg;
    int shift = ch->sweepShift;
    int delta = reg >> shift;
    int nextReg = ch->sweepSubtract ? reg - delta : reg + delta;
    if (nextReg > 0x7FF || nextReg < 0) return -1;
    if (ch->sweepPace == 0) return reg;
    int iterations = (int)floorf(elapsed * 128.0f / ch->sweepPace);
    for (int i = 0; i < iterations; i++) {
        reg = nextReg;
        delta = reg >> shift;
        nextReg = ch->sweepSubtract ? reg - delta : reg + delta;
        if (nextReg > 0x7FF || nextReg < 0) return -1;
    }
    return reg;
}

// sampleNoise: advance the channel LFSR and return the noise bit.
static float noise_sample(Synth* s, ChanState* ch, int parameter) {
    float divisor = NOISE_DIVISORS[parameter & 7];
    int shift = (parameter >> 4) & 0xF;
    if (shift < 14) {
        float pitch = s->pitch[ch->hw - 1];
        float cycles = (float)GB_CLOCK / divisor
            / powf(2.0f, (float)shift) / (float)s->rate * pitch;
        int width7 = (parameter & 8) != 0;
        float remaining = cycles;
        while (remaining > 0) {
            float untilClock = 1.0f - ch->noiseClock;
            float span = remaining < untilClock ? remaining : untilClock;
            ch->noiseClock += span;
            remaining -= span;
            if (ch->noiseClock >= 1.0f - 1e-12f) {
                ch->noiseClock = 0;
                clock_noise(ch, width7);
            }
        }
    }
    return (ch->noiseLfsr & 1) == 0 ? 1.0f : 0.0f;
}

// Advance a drum to the next segment (resetting the noise LFSR on change,
// matching sampleDrum).  Returns 0 when the drum is exhausted.
static int advance_drum_segment(lua_State* L, ChanState* ch, DrumState* d) {
    int start, end, vol, fade, param;
    if (!read_segment(L, d->drumRef, d->segIndex + 1,
                      &start, &end, &vol, &fade, &param)) {
        d->segValid = 0;
        return 0;
    }
    d->segIndex = d->segIndex + 1;
    d->segStart = start;
    d->segEnd = end;
    d->segVolume = vol;
    d->segFade = fade;
    d->segParameter = param;
    d->segValid = 1;
    reset_noise(ch);
    return 1;
}

// Render one drum sample at within-drum index sampleIndex (0-based).
static float render_drum(lua_State* L, Synth* s, ChanState* ch, DrumState* d,
                         int sampleIndex) {
    if (d->segValid) {
        while (sampleIndex >= d->segEnd) {
            if (!advance_drum_segment(L, ch, d)) return 0;
        }
    } else {
        // locate the segment covering sampleIndex without resetting noise
        int start, end, vol, fade, param;
        int seg = 1;
        while (read_segment(L, d->drumRef, seg,
                            &start, &end, &vol, &fade, &param)) {
            if (sampleIndex < end) {
                d->segIndex = seg;
                d->segStart = start;
                d->segEnd = end;
                d->segVolume = vol;
                d->segFade = fade;
                d->segParameter = param;
                d->segValid = 1;
                break;
            }
            seg++;
        }
        if (!d->segValid) return 0;
    }
    if (sampleIndex < d->segStart) return 0;
    float elapsed = (float)(sampleIndex - d->segStart) / (float)s->rate;
    float volume = envelope((float)d->segVolume, d->segFade, elapsed);
    return noise_sample(s, ch, d->segParameter) * volume / 15.0f;
}

static void apply_pan(ChanState* ch, int stereo, float v,
                      float* mixL, float* mixR) {
    if (!stereo) {
        *mixL += v;
        return;
    }
    if (ch->panLeft) *mixL += v;
    if (ch->panRight) *mixR += v;
}

static void render_channel(lua_State* L, Synth* s, int ci, int stereo,
                           float* mixL, float* mixR) {
    ChanState* ch = &s->channels[ci];
    if (ch->ended) {
        if (ch->hasTail) {
            float gain = s->vol[ch->hw - 1];
            float v = render_drum(L, s, ch, &ch->tail, ch->tailSample) * gain;
            ch->tailSample++;
            if (ch->tailSample >= ch->tailEnd) ch->hasTail = 0;
            *mixL += v;
            *mixR += v;
        }
        return;
    }
    int sampleIndex = ch->eventSample;
    ch->eventSample = sampleIndex + 1;
    float elapsed = (float)sampleIndex / (float)s->rate;
    int frame = (int)(elapsed * 60.0f);

    if (ch->silence) {
        if (ch->hasTail) {
            float gain = s->vol[ch->hw - 1];
            float v = render_drum(L, s, ch, &ch->tail, ch->tailSample) * gain;
            ch->tailSample++;
            if (ch->tailSample >= ch->tailEnd) ch->hasTail = 0;
            *mixL += v;
            *mixR += v;
        }
        return;
    }
    if (ch->drum) {
        float gain = s->vol[ch->hw - 1];
        float v = render_drum(L, s, ch, &ch->cur, sampleIndex) * gain;
        apply_pan(ch, stereo, v, mixL, mixR);
        return;
    }
    ch->hasTail = 0;
    float gain = s->vol[ch->hw - 1];
    float volume = envelope(ch->volume, ch->fade, elapsed);
    if (ch->noise) {
        float v = noise_sample(s, ch, ch->noiseParameter) * volume / 15.0f * gain;
        apply_pan(ch, stereo, v, mixL, mixR);
        return;
    }
    float reg = (float)ch->reg;
    if (ch->hasSweep) {
        int r = swept_register(ch, ch->reg, elapsed);
        if (r < 0) return;
        reg = (float)r;
    } else if (ch->hasSlide) {
        float amount = frame >= ch->slideFrames
            ? 1.0f : (float)frame / (float)ch->slideFrames;
        reg = reg + (ch->slideTarget - reg) * amount;
    } else if (ch->hasVibrato && frame >= ch->vibratoDelay) {
        int toggles = (frame - ch->vibratoDelay + 1) / (ch->vibratoRate + 1);
        if (toggles > 0) {
            int r = (int)reg;
            int low = r & 0xFF;
            int high = r & 0x700;
            if (toggles & 1) {
                int l = low + ch->vibratoAbove;
                reg = (float)(high + (l > 0xFF ? 0xFF : l));
            } else {
                int l = low - ch->vibratoBelow;
                reg = (float)(high + (l < 0 ? 0 : l));
            }
        }
    }
    if (reg > 2047) reg = 2047;
    float pitch = s->pitch[ch->hw - 1];
    float frequency = 131072.0f / (2048.0f - reg) * pitch;
    if (ch->wave) frequency *= 0.5f;
    float phase = ch->phase;
    ch->phase += frequency / (float)s->rate;
    if (ch->phase >= 1.0f) ch->phase -= 1.0f;
    float out;
    if (ch->wave) {
        if (!ch->waveValid) return;
        int index = (int)(phase * 32.0f);
        if (index > 31) index = 31;
        float nibble = (float)ch->waveTable[index];
        out = (nibble / 15.0f) * ch->waveLevel * gain;
    } else {
        int duty;
        if (ch->hasDutyPattern) duty = ch->dutyPattern[frame % 4];
        else duty = ch->duty;
        if (duty < 0) duty = 0;
        if (duty > 3) duty = 2;
        int step = ((int)(phase * 8.0f)) & 7;
        out = DUTY[duty][step] ? (volume / 15.0f) * gain : 0.0f;
    }
    apply_pan(ch, stereo, out, mixL, mixR);
}

static float analog_out(Synth* s, int side, float input) {
    float cap = s->hpfCap[side];
    float hp = input - cap;
    s->hpfCap[side] = input - hp * s->hpfCharge;
    float prev = s->lpf[side];
    float lp = prev + LPF_ALPHA * (hp - prev);
    s->lpf[side] = lp;
    float v = lp * MIX_SCALE;
    if (v > 1.0f) v = 1.0f;
    else if (v < -1.0f) v = -1.0f;
    return v;
}

static int l_synth_render(lua_State* L) {
    Synth* s = (Synth*)luaL_checkudata(L, 1, "Synth");
    struct SoundData* sd = (struct SoundData*)luaL_checkudata(L, 2, "SoundData");
    int offset = (int)luaL_checkinteger(L, 3);
    int count = (int)luaL_checkinteger(L, 4);
    int stereo = lua_toboolean(L, 5);
    if (offset < 0) offset = 0;
    int maxFrames = sd->samples - offset;
    if (count > maxFrames) count = maxFrames;
    if (count <= 0) {
        lua_pushinteger(L, 0);
        return 1;
    }
    s16* out = sd->data;
    int nch = sd->channels;
    int rendered = 0;
    for (int i = 0; i < count; i++) {
        int stop = 0;
        for (int ci = 0; ci < s->nch; ci++) {
            ChanState* ch = &s->channels[ci];
            if (ch->active && ch->eventSample >= ch->eventSamples) {
                stop = 1;
                break;
            }
        }
        if (stop) break;
        int silent = 1;
        for (int ci = 0; ci < s->nch; ci++) {
            ChanState* ch = &s->channels[ci];
            if (ch->active || ch->hasTail) {
                silent = 0;
                break;
            }
        }
        if (silent) break;
        float mixL = 0, mixR = 0;
        for (int ci = 0; ci < s->nch; ci++) {
            render_channel(L, s, ci, stereo, &mixL, &mixR);
        }
        if (stereo) {
            float l = analog_out(s, 0, mixL);
            float r = analog_out(s, 1, mixR);
            out[(offset + i) * nch] = (s16)(l * 32767.0f);
            if (nch >= 2) out[(offset + i) * nch + 1] = (s16)(r * 32767.0f);
        } else {
            float v = analog_out(s, 0, mixL);
            if (nch >= 2) {
                out[(offset + i) * nch] = (s16)(v * 32767.0f);
                out[(offset + i) * nch + 1] = (s16)(v * 32767.0f);
            } else {
                out[(offset + i) * nch] = (s16)(v * 32767.0f);
            }
        }
        rendered++;
    }
    lua_pushinteger(L, rendered);
    return 1;
}

static void synth_unref_drum(lua_State* L, ChanState* ch) {
    if (ch->cur.drumRef) {
        luaL_unref(L, LUA_REGISTRYINDEX, ch->cur.drumRef);
        ch->cur.drumRef = 0;
    }
    if (ch->tail.drumRef) {
        luaL_unref(L, LUA_REGISTRYINDEX, ch->tail.drumRef);
        ch->tail.drumRef = 0;
    }
    if (ch->tailRef) {
        luaL_unref(L, LUA_REGISTRYINDEX, ch->tailRef);
        ch->tailRef = 0;
    }
}

// setEvent(ch, event, tailDrum, tailSample, waveTable)
static int l_synth_setEvent(lua_State* L) {
    Synth* s = (Synth*)luaL_checkudata(L, 1, "Synth");
    int ci = (int)luaL_checkinteger(L, 2) - 1;
    if (ci < 0 || ci >= s->nch) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ChanState* ch = &s->channels[ci];
    // If the tail arg is the same drum table already ringing, keep the C's
    // sample counter and segment state (Lua does not advance drumTail.sample
    // while the C renders); otherwise a fresh tail is (re)established.
    int sameTail = 0;
    if (ch->tailRef != 0 && lua_istable(L, 4)) {
        lua_pushinteger(L, ch->tailRef); lua_gettable(L, LUA_REGISTRYINDEX);
        sameTail = lua_rawequal(L, -1, 4);
        lua_pop(L, 1);
    }
    synth_unref_drum(L, ch);
    ch->hasTail = 0;

    int tailSample = (int)luaL_optinteger(L, 5, 0);
    // set up the tail first so it can reuse the drum state cleanly
    if (!lua_isnil(L, 4)) {
        lua_pushvalue(L, 4);
        ch->tailRef = luaL_ref(L, LUA_REGISTRYINDEX);
        ch->hasTail = 1;
        if (!sameTail) {
            ch->tailSample = tailSample;
            lua_getfield(L, 4, "drum");
            ch->tail.drumRef = luaL_ref(L, LUA_REGISTRYINDEX);
            ch->tail.segValid = 0;
            ch->tail.segIndex = 0;
            ch->tailEnd = 0;
            // scan for the segment covering tailSample (no noise reset) + tailEnd
            int start, end, vol, fade, param;
            int seg = 1;
            while (read_segment(L, ch->tail.drumRef, seg,
                                &start, &end, &vol, &fade, &param)) {
                ch->tailEnd = end;
                if (!ch->tail.segValid && tailSample < end) {
                    ch->tail.segIndex = seg;
                    ch->tail.segStart = start;
                    ch->tail.segEnd = end;
                    ch->tail.segVolume = vol;
                    ch->tail.segFade = fade;
                    ch->tail.segParameter = param;
                    ch->tail.segValid = 1;
                }
                seg++;
            }
        }
    }

    if (lua_isnil(L, 3)) {
        ch->ended = 1;
        ch->active = 0;
        lua_pushboolean(L, 1);
        return 1;
    }

    luaL_checktype(L, 3, LUA_TTABLE);
    int eventIdx = 3;
    ch->ended = 0;
    ch->active = 1;
    ch->eventSample = 0;
    ch->eventSamples = (int)luaL_getfield_int_or_default(L, eventIdx, "samples", 0);
    ch->phase = 0;
    ch->silence = luaL_getfield_bool(L, eventIdx, "silence", 0);
    ch->noise = luaL_getfield_bool(L, eventIdx, "noise", 0);
    ch->wave = luaL_getfield_bool(L, eventIdx, "wave", 0);
    ch->reg = (int)luaL_getfield_int_or_default(L, eventIdx, "register", 0);
    ch->volume = (float)luaL_getfield_num_or_default(L, eventIdx, "volume", 0);
    ch->fade = (int)luaL_getfield_int_or_default(L, eventIdx, "fade", 0);
    ch->noiseParameter = (int)luaL_getfield_int_or_default(L, eventIdx, "noiseParameter", 0);
    ch->waveInstrument = (int)luaL_getfield_int_or_default(L, eventIdx, "waveInstrument", 0);
    ch->waveLevel = luaL_getfield_num_or_default(L, eventIdx, "waveLevel", 1);
    ch->panLeft = luaL_getfield_pan(L, eventIdx, "panLeft");
    ch->panRight = luaL_getfield_pan(L, eventIdx, "panRight");
    ch->hasSweep = 0;
    ch->hasSlide = 0;
    ch->hasVibrato = 0;

    // duty: number or 4-element pattern table
    ch->hasDutyPattern = 0;
    lua_getfield(L, eventIdx, "duty");
    if (lua_isnil(L, -1)) {
        ch->duty = 2;
    } else if (lua_istable(L, -1)) {
        ch->hasDutyPattern = 1;
        for (int k = 0; k < 4; k++) {
            lua_pushinteger(L, k + 1); lua_gettable(L, -2);
            ch->dutyPattern[k] = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    } else {
        ch->duty = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    // sweep
    lua_getfield(L, eventIdx, "sweep");
    if (!lua_isnil(L, -1)) {
        ch->hasSweep = 1;
        ch->sweepPace = (int)luaL_getfield_int_or_default(L, -1, "pace", 0);
        ch->sweepSubtract = luaL_getfield_bool(L, -1, "subtract", 0);
        ch->sweepShift = (int)luaL_getfield_int_or_default(L, -1, "shift", 0);
    }
    lua_pop(L, 1);

    // slide
    lua_getfield(L, eventIdx, "slide");
    if (!lua_isnil(L, -1)) {
        ch->hasSlide = 1;
        ch->slideTarget = (float)luaL_getfield_num_or_default(L, -1, "target", 0);
        ch->slideFrames = (int)luaL_getfield_int_or_default(L, -1, "frames", 1);
    }
    lua_pop(L, 1);

    // vibrato
    lua_getfield(L, eventIdx, "vibrato");
    if (!lua_isnil(L, -1)) {
        ch->hasVibrato = 1;
        ch->vibratoDelay = (int)luaL_getfield_int_or_default(L, -1, "delay", 0);
        ch->vibratoRate = (int)luaL_getfield_int_or_default(L, -1, "rate", 0);
        ch->vibratoAbove = (int)luaL_getfield_int_or_default(L, -1, "above", 0);
        ch->vibratoBelow = (int)luaL_getfield_int_or_default(L, -1, "below", 0);
    }
    lua_pop(L, 1);

    // drum
    lua_getfield(L, eventIdx, "drum");
    if (lua_isnil(L, -1)) {
        ch->drum = 0;
        lua_pop(L, 1);
        ch->cur.drumRef = 0;
        ch->cur.segValid = 0;
    } else {
        ch->drum = 1;
        ch->cur.drumRef = luaL_ref(L, LUA_REGISTRYINDEX);
        ch->cur.segIndex = 0;
        ch->cur.segValid = 0;
        reset_noise(ch);
    }

    // Lua resets the noise LFSR when a non-drum event arrives without a
    // ringing tail (Channel:sample advance rules).
    if (!ch->drum && lua_isnil(L, 4)) reset_noise(ch);

    // wave table (32 nibbles 0-15), passed explicitly as arg 6 from Lua
    ch->waveValid = 0;
    if (ch->wave && lua_istable(L, 6)) {
        for (int k = 0; k < 32; k++) {
            lua_pushinteger(L, k + 1); lua_gettable(L, 6);
            float value = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            float nibble = value * 8.0f + 8.0f;
            if (nibble < 0) nibble = 0;
            if (nibble > 15) nibble = 15;
            ch->waveTable[k] = nibble;
        }
        ch->waveValid = 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_synth_resetNoise(lua_State* L) {
    Synth* s = (Synth*)luaL_checkudata(L, 1, "Synth");
    int ci = (int)luaL_checkinteger(L, 2) - 1;
    if (ci >= 0 && ci < s->nch) reset_noise(&s->channels[ci]);
    return 0;
}

static int l_synth_eventSample(lua_State* L) {
    Synth* s = (Synth*)luaL_checkudata(L, 1, "Synth");
    int ci = (int)luaL_checkinteger(L, 2) - 1;
    if (ci < 0 || ci >= s->nch) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, s->channels[ci].eventSample);
    return 1;
}

static int l_synth_setChannelVolumes(lua_State* L) {
    Synth* s = (Synth*)luaL_checkudata(L, 1, "Synth");
    luaL_checktype(L, 2, LUA_TTABLE);
    for (int k = 0; k < 4; k++) {
        lua_pushinteger(L, k + 1); lua_gettable(L, 2);
        s->vol[k] = (float)lua_tonumber(L, -1);
        if (s->vol[k] < 0) s->vol[k] = 0;
        lua_pop(L, 1);
    }
    return 0;
}

static int l_synth_setChannelPitches(lua_State* L) {
    Synth* s = (Synth*)luaL_checkudata(L, 1, "Synth");
    luaL_checktype(L, 2, LUA_TTABLE);
    for (int k = 0; k < 4; k++) {
        lua_pushinteger(L, k + 1); lua_gettable(L, 2);
        s->pitch[k] = (float)lua_tonumber(L, -1);
        if (s->pitch[k] < 0) s->pitch[k] = 0;
        lua_pop(L, 1);
    }
    return 0;
}

static int l_synth_gc(lua_State* L) {
    Synth* s = (Synth*)luaL_checkudata(L, 1, "Synth");
    if (s->channels) {
        for (int ci = 0; ci < s->nch; ci++) {
            synth_unref_drum(L, &s->channels[ci]);
        }
        free(s->channels);
        s->channels = NULL;
    }
    return 0;
}

// love.synth.new(rate, {hw1, hw2, ...})
static int l_synth_new(lua_State* L) {
    int rate = (int)luaL_checkinteger(L, 1);
    if (rate <= 0) rate = 22050;
    int nch = 0;
    if (lua_istable(L, 2)) {
        nch = (int)lua_objlen(L, 2);
        if (nch > SYNTH_MAX_CHANNELS) nch = SYNTH_MAX_CHANNELS;
    }
    if (nch <= 0) nch = 4;
    Synth* s = (Synth*)lua_newuserdata(L, sizeof(Synth));
    memset(s, 0, sizeof(Synth));
    s->rate = rate;
    s->nch = nch;
    s->channels = (ChanState*)calloc(nch, sizeof(ChanState));
    if (!s->channels) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    for (int ci = 0; ci < nch; ci++) {
        ChanState* ch = &s->channels[ci];
        ch->hw = 1;
        if (lua_istable(L, 2)) {
            lua_pushinteger(L, ci + 1); lua_gettable(L, 2);
            int hw = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
            if (hw >= 1 && hw <= 4) ch->hw = hw;
        }
        ch->duty = 2;
        ch->phase = 0;
        reset_noise(ch);
    }
    for (int k = 0; k < 4; k++) {
        s->vol[k] = 1.0f;
        s->pitch[k] = 1.0f;
    }
    s->hpfCharge = powf(0.999958f, (float)GB_CLOCK / (float)rate);
    luaL_getmetatable(L, "Synth");
    lua_setmetatable(L, -2);
    return 1;
}

static const luaL_Reg synth_methods[] = {
    {"render", l_synth_render},
    {"setEvent", l_synth_setEvent},
    {"resetNoise", l_synth_resetNoise},
    {"eventSample", l_synth_eventSample},
    {"setChannelVolumes", l_synth_setChannelVolumes},
    {"setChannelPitches", l_synth_setChannelPitches},
    {"__gc", l_synth_gc},
    {NULL, NULL}
};

static const luaL_Reg synth_funcs[] = {
    {"new", l_synth_new},
    {NULL, NULL}
};

extern "C" int luaopen_love_synth(lua_State* L) {
    luaL_newmetatable(L, "Synth");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_register(L, NULL, synth_methods);
    lua_pop(L, 1);
    luaL_register(L, "love.synth", synth_funcs);
    return 1;
}
