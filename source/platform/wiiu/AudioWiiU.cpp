/* -----------------------------------------------------------------------
 * AudioWiiU — ASND, MP3Player and the player's stream mixed into one
 * output, see AudioWiiU.h.
 *
 * SDL's audio (the console's AX underneath) asks for 48 kHz stereo; its
 * callback mixes: the 16 voices (resampled from their own rate), the MP3
 * being played (decoded by libmad on a thread into a ring) and the
 * player's stream (already 48 kHz, its times kept to tell the clock).
 * Samples are the CPU's s16 (big-endian, as the Wii's ASND wants them).
 * ----------------------------------------------------------------------- */
#include "AudioWiiU.h"

#include <SDL2/SDL.h>
#include <mad.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <mutex>
#include <thread>

#include <asndlib.h>
#include <mp3player.h>
#include <ogc/system.h>

namespace {

SDL_AudioDeviceID s_dev = 0;
bool s_tried = false;
int  s_bufFrames = 1024;
volatile uint32_t s_lastCallbackMs = 0;   /* (32 bits: no atomics library) */

uint64_t nowUs() { return (uint64_t)OSTicksToMicroseconds(OSGetSystemTime()); }
uint32_t nowMs32() { return (uint32_t)OSTicksToMilliseconds(OSGetSystemTime()); }

/* ---- ASND's voices ---- */
struct Voice {
    const uint8_t* data = nullptr;
    int      frames = 0;             /* samples per channel */
    bool     stereo = false, bits16 = true, loop = false, paused = false;
    uint32_t pos = 0;                /* 16.16 frames */
    uint32_t step = 0;               /* 16.16, the voice's rate / 48 kHz */
    int      volL = 255, volR = 255;
    bool     on = false;
};
Voice s_voices[MAX_SND_VOICES];
bool  s_asndPaused = false;

/* ---- MP3Player: a ring of stereo frames at the MP3's rate ---- */
const int MP3_RING = 32768;
int16_t   s_mp3Ring[MP3_RING * 2];
volatile int s_mp3Rd = 0, s_mp3Wr = 0;   /* frames, ever (mod MP3_RING) */
volatile int s_mp3Rate = 44100;
uint32_t  s_mp3Frac = 0;
volatile bool s_mp3Decoding = false, s_mp3Stop = false;
float     s_mp3Vol = 1.0f;
std::thread* s_mp3Thread = nullptr;
const uint8_t* s_mp3Data = nullptr;
int32_t   s_mp3Len = 0;

/* ---- the player's stream (48 kHz stereo) ---- */
int16_t*  s_ring = nullptr;
int       s_ringCap = 0;              /* frames */
uint64_t  s_rd = 0, s_wr = 0;         /* frames, ever */
struct Mark { uint64_t at; double pts; };
std::deque<Mark> s_marks;
bool      s_streamOn = false, s_streamPaused = false;
float     s_streamVol = 0.7f;
int       s_aheadFrames = 48000;
uint64_t  s_simUs = 0;                /* no output: time consumed by the clock */

/* the mix's last samples, mono (the music player's bars) */
const int RECENT = 4096;
float     s_recent[RECENT];
int       s_recentPos = 0;

inline int32_t voiceSample(const Voice& v, int i, int ch)
{
    if (v.bits16) {
        const int16_t* p = (const int16_t*)v.data;
        return v.stereo ? p[i * 2 + ch] : p[i];
    }
    const int8_t* p = (const int8_t*)v.data;
    return (v.stereo ? p[i * 2 + ch] : p[i]) * 256;
}

void mix(void*, Uint8* out8, int len)
{
    s_lastCallbackMs = nowMs32();
    int16_t* out = (int16_t*)out8;
    const int frames = len / 4;
    static int32_t acc[8192 * 2];
    const int n = frames > 8192 ? 8192 : frames;
    memset(acc, 0, sizeof(int32_t) * n * 2);

    if (!s_asndPaused) {
        for (Voice& v : s_voices) {
            if (!v.on || v.paused || !v.data || v.frames <= 0) continue;
            for (int i = 0; i < n; ++i) {
                int idx = (int)(v.pos >> 16);
                if (idx >= v.frames) {
                    if (!v.loop) { v.on = false; break; }
                    v.pos -= (uint32_t)v.frames << 16;
                    idx = (int)(v.pos >> 16);
                }
                acc[i * 2]     += voiceSample(v, idx, 0) * v.volL / 255;
                acc[i * 2 + 1] += voiceSample(v, idx, v.stereo ? 1 : 0) * v.volR / 255;
                v.pos += v.step;
            }
        }
    }

    /* the menus' music */
    {
        const uint32_t step = (uint32_t)(((uint64_t)s_mp3Rate << 16) / AudioWiiU::RATE);
        const int vol = (int)(s_mp3Vol * 256);
        for (int i = 0; i < n; ++i) {
            if (s_mp3Rd == s_mp3Wr) break;
            const int k = s_mp3Rd % MP3_RING;
            acc[i * 2]     += s_mp3Ring[k * 2] * vol >> 8;
            acc[i * 2 + 1] += s_mp3Ring[k * 2 + 1] * vol >> 8;
            s_mp3Frac += step;
            while (s_mp3Frac >= 0x10000 && s_mp3Rd != s_mp3Wr) { s_mp3Frac -= 0x10000; ++s_mp3Rd; }
        }
    }

    /* the player's stream */
    if (s_streamOn && !s_streamPaused && s_ring) {
        const int vol = (int)(s_streamVol * 256);
        int have = (int)(s_wr - s_rd);
        const int take = have < n ? have : n;
        for (int i = 0; i < take; ++i) {
            const int k = (int)((s_rd + i) % s_ringCap);
            acc[i * 2]     += s_ring[k * 2] * vol >> 8;
            acc[i * 2 + 1] += s_ring[k * 2 + 1] * vol >> 8;
        }
        s_rd += take;
    }

    for (int i = 0; i < n * 2; ++i) {
        int32_t s = acc[i];
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    for (int i = n * 2; i < frames * 2; ++i) out[i] = 0;
    for (int i = 0; i < n; i += 4) {
        s_recent[s_recentPos] = (out[i * 2] + out[i * 2 + 1]) * (1.0f / 65536.0f);
        s_recentPos = (s_recentPos + 1) % RECENT;
    }
}

struct Lock {
    Lock() { if (s_dev) SDL_LockAudioDevice(s_dev); }
    ~Lock() { if (s_dev) SDL_UnlockAudioDevice(s_dev); }
};

/* the output asks for nothing (an emulator without sound): the stream is
 * heard at the clock's pace all the same */
bool outputDead() { return !s_dev || (uint32_t)(nowMs32() - s_lastCallbackMs) > 300; }
void simulate()
{
    if (!outputDead()) { s_simUs = 0; return; }
    const uint64_t t = nowUs();
    if (!s_simUs) { s_simUs = t; return; }
    if (s_streamPaused) { s_simUs = t; return; }
    uint64_t f = (t - s_simUs) * AudioWiiU::RATE / 1000000;
    if (f == 0) return;
    s_simUs += f * 1000000 / AudioWiiU::RATE;
    Lock l;
    const uint64_t have = s_wr - s_rd;
    s_rd += f < have ? f : have;
}

/* ---- libmad ---- */
inline int16_t toS16(mad_fixed_t s)
{
    s += 1L << (MAD_F_FRACBITS - 16);
    if (s >= MAD_F_ONE) s = MAD_F_ONE - 1;
    else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
    return (int16_t)(s >> (MAD_F_FRACBITS + 1 - 16));
}

void mp3Main()
{
    mad_stream stream;
    mad_frame frame;
    mad_synth synth;
    mad_stream_init(&stream);
    mad_frame_init(&frame);
    mad_synth_init(&synth);
    mad_stream_buffer(&stream, s_mp3Data, s_mp3Len);
    bool rateSet = false;
    while (!s_mp3Stop) {
        if (mad_frame_decode(&frame, &stream) == -1) {
            if (MAD_RECOVERABLE(stream.error)) continue;
            break;
        }
        mad_synth_frame(&synth, &frame);
        if (!rateSet) { s_mp3Rate = synth.pcm.samplerate ? synth.pcm.samplerate : 44100; rateSet = true; }
        const mad_pcm& pcm = synth.pcm;
        for (unsigned i = 0; i < pcm.length && !s_mp3Stop; ++i) {
            while (s_mp3Wr - s_mp3Rd >= MP3_RING - 1 && !s_mp3Stop) OSSleepTicks(OSMillisecondsToTicks(5));
            const int k = s_mp3Wr % MP3_RING;
            const int16_t l = toS16(pcm.samples[0][i]);
            s_mp3Ring[k * 2] = l;
            s_mp3Ring[k * 2 + 1] = pcm.channels == 2 ? toS16(pcm.samples[1][i]) : l;
            ++s_mp3Wr;
        }
    }
    mad_synth_finish(&synth);
    mad_frame_finish(&frame);
    mad_stream_finish(&stream);
    /* what was decoded, heard */
    while (!s_mp3Stop && s_mp3Rd != s_mp3Wr && !outputDead()) OSSleepTicks(OSMillisecondsToTicks(10));
    s_mp3Decoding = false;
}

} // namespace

namespace AudioWiiU {

bool init()
{
    if (s_dev) return true;
    if (s_tried) return false;
    s_tried = true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SYS_Report("[AudioWiiU] SDL audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix;
    s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!s_dev) {
        SYS_Report("[AudioWiiU] no audio device: %s\n", SDL_GetError());
        return false;
    }
    s_bufFrames = have.samples;
    SDL_PauseAudioDevice(s_dev, 0);
    SYS_Report("[AudioWiiU] output %d Hz, %d ch, %d frames a buffer\n", have.freq, have.channels, have.samples);
    return true;
}

void shutdown()
{
    MP3Player_Stop();
    if (s_dev) {
        SDL_PauseAudioDevice(s_dev, 1);
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    streamStop();
}

void streamStart(double ahead)
{
    init();
    streamStop();
    const int cap = (int)(ahead * RATE) + RATE;
    int16_t* r = (int16_t*)malloc((size_t)cap * 4);
    Lock l;
    s_ring = r;
    s_ringCap = r ? cap : 0;
    s_rd = s_wr = 0;
    s_marks.clear();
    s_aheadFrames = (int)(ahead * RATE);
    s_streamPaused = false;
    s_streamOn = r != nullptr;
    s_simUs = 0;
}

void streamStop()
{
    int16_t* r;
    {
        Lock l;
        s_streamOn = false;
        r = s_ring;
        s_ring = nullptr;
        s_ringCap = 0;
        s_rd = s_wr = 0;
        s_marks.clear();
    }
    free(r);
}

bool streamPush(const int16_t* pcm, int frames, double pts, volatile bool* stop)
{
    if (!s_streamOn) return false;
    {
        Lock l;
        s_marks.push_back({ s_wr, pts });
        while (s_marks.size() > 512) s_marks.pop_front();
    }
    while (frames > 0) {
        simulate();
        int room;
        {
            Lock l;
            room = s_aheadFrames - (int)(s_wr - s_rd);
            if (room > 0) {
                const int k = room < frames ? room : frames;
                for (int i = 0; i < k; ++i) {
                    const int at = (int)((s_wr + i) % s_ringCap);
                    s_ring[at * 2] = pcm[i * 2];
                    s_ring[at * 2 + 1] = pcm[i * 2 + 1];
                }
                s_wr += k;
                pcm += k * 2;
                frames -= k;
                continue;
            }
        }
        if (stop && *stop) return false;
        OSSleepTicks(OSMillisecondsToTicks(4));
    }
    return true;
}

int streamQueued()
{
    simulate();
    Lock l;
    return (int)(s_wr - s_rd);
}

double streamClock()
{
    if (!s_streamOn) return -1;
    simulate();
    Lock l;
    if (s_marks.empty() || s_rd == 0) return -1;
    /* what the output plays now: what it took, less its buffer */
    const double lag = outputDead() ? 0.0 : s_bufFrames * 1.5;
    const double heard = (double)s_rd - lag;
    const Mark* m = &s_marks.front();
    for (const Mark& k : s_marks) {
        if ((double)k.at <= heard) m = &k;
        else break;
    }
    while (s_marks.size() > 1 && (double)s_marks[1].at <= heard - RATE) s_marks.pop_front();
    return m->pts + (heard - (double)m->at) / RATE;
}

void streamFlush()
{
    Lock l;
    s_rd = s_wr = 0;
    s_marks.clear();
    s_simUs = 0;
}

void streamPause(bool p)
{
    Lock l;
    s_streamPaused = p;
}

void streamVolume(float v)
{
    Lock l;
    s_streamVol = v < 0 ? 0 : v > 1 ? 1 : v;
}

int recent(float* out, int n)
{
    if (n > RECENT) n = RECENT;
    Lock l;
    for (int i = 0; i < n; ++i) out[i] = s_recent[(s_recentPos - n + i + RECENT) % RECENT];
    return n;
}

}

/* ---- ASND ------------------------------------------------------------------ */

extern "C" {

void ASND_Init(void) { AudioWiiU::init(); }

void ASND_End(void)
{
    Lock l;
    for (Voice& v : s_voices) v.on = false;
}

void ASND_Pause(s32 paused)
{
    Lock l;
    s_asndPaused = paused != 0;
}

s32 ASND_Is_Paused(void) { return s_asndPaused; }

static s32 setVoice(s32 voice, s32 format, s32 pitch, void* snd, s32 size, s32 vl, s32 vr, bool loop)
{
    if (voice < 0 || voice >= MAX_SND_VOICES || !snd || size <= 0 || pitch <= 0) return SND_INVALID;
    if (!AudioWiiU::init()) return SND_INVALID;
    const bool stereo = format == VOICE_STEREO_16BIT || format == VOICE_STEREO_8BIT;
    const bool bits16 = format == VOICE_MONO_16BIT || format == VOICE_STEREO_16BIT;
    Lock l;
    Voice& v = s_voices[voice];
    v.data = (const uint8_t*)snd;
    v.frames = size / ((stereo ? 2 : 1) * (bits16 ? 2 : 1));
    v.stereo = stereo;
    v.bits16 = bits16;
    v.loop = loop;
    v.paused = false;
    v.pos = 0;
    v.step = (uint32_t)(((uint64_t)pitch << 16) / AudioWiiU::RATE);
    v.volL = vl; v.volR = vr;
    v.on = true;
    return SND_OK;
}

s32 ASND_SetVoice(s32 voice, s32 format, s32 pitch, s32 delay, void* snd, s32 size,
                  s32 vl, s32 vr, ASNDVoiceCallback cb)
{
    (void)delay; (void)cb;
    return setVoice(voice, format, pitch, snd, size, vl, vr, false);
}

s32 ASND_SetInfiniteVoice(s32 voice, s32 format, s32 pitch, s32 delay, void* snd, s32 size,
                          s32 vl, s32 vr)
{
    (void)delay;
    return setVoice(voice, format, pitch, snd, size, vl, vr, true);
}

s32 ASND_StopVoice(s32 voice)
{
    if (voice < 0 || voice >= MAX_SND_VOICES) return SND_INVALID;
    Lock l;
    s_voices[voice].on = false;
    return SND_OK;
}

s32 ASND_PauseVoice(s32 voice, s32 pause)
{
    if (voice < 0 || voice >= MAX_SND_VOICES) return SND_INVALID;
    Lock l;
    s_voices[voice].paused = pause != 0;
    return SND_OK;
}

s32 ASND_StatusVoice(s32 voice)
{
    if (voice < 0 || voice >= MAX_SND_VOICES) return SND_INVALID;
    if (!s_dev) return SND_UNUSED;
    if (outputDead()) {   /* no output: a sound is over at once */
        Lock l;
        s_voices[voice].on = false;
    }
    return s_voices[voice].on ? SND_WORKING : SND_UNUSED;
}

s32 ASND_ChangeVolumeVoice(s32 voice, s32 vl, s32 vr)
{
    if (voice < 0 || voice >= MAX_SND_VOICES) return SND_INVALID;
    Lock l;
    s_voices[voice].volL = vl;
    s_voices[voice].volR = vr;
    return SND_OK;
}

s32 ASND_GetFirstUnusedVoice(void)
{
    for (int v = 1; v < MAX_SND_VOICES; ++v)
        if (ASND_StatusVoice(v) == SND_UNUSED) return v;
    return SND_INVALID;
}

/* ---- MP3Player -------------------------------------------------------------- */

void MP3Player_Init(void) { AudioWiiU::init(); }

void MP3Player_Stop(void)
{
    if (s_mp3Thread) {
        s_mp3Stop = true;
        s_mp3Thread->join();
        delete s_mp3Thread;
        s_mp3Thread = nullptr;
    }
    Lock l;
    s_mp3Rd = s_mp3Wr = 0;
    s_mp3Decoding = false;
}

s32 MP3Player_PlayBuffer(const void* buffer, s32 len, void (*filter)(void*, s32*))
{
    (void)filter;
    MP3Player_Stop();
    if (!AudioWiiU::init() || !buffer || len <= 0) return -1;
    s_mp3Data = (const uint8_t*)buffer;
    s_mp3Len = len;
    s_mp3Stop = false;
    s_mp3Decoding = true;
    s_mp3Thread = new std::thread(mp3Main);
    return 0;
}

bool MP3Player_IsPlaying(void) { return s_mp3Decoding; }

void MP3Player_Volume(u32 volume) { s_mp3Vol = volume / 255.0f; }

} // extern "C"
