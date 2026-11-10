/* -----------------------------------------------------------------------
 * ASND and MP3Player (libogc) on the 3DS's DSP (ndsp).
 *
 * ASND's voices 0..15 are ndsp channels 0..15.  The DSP reads samples from
 * linear memory: a sound given in ordinary memory is copied there once
 * (kept by address: SoundFX plays the same buffers again and again).
 * MP3Player decodes with libmad on a thread into channel 16.  The video
 * player (Player3ds) uses channel 17.
 * ----------------------------------------------------------------------- */
#include <3ds.h>
#include <mad.h>
#include <stdlib.h>
#include <string.h>
#include <map>

#include <asndlib.h>
#include <mp3player.h>
#include <ogc/system.h>

namespace {

bool s_ndsp = false;
bool s_ndspFailed = false;
bool s_paused = false;

struct Voice {
    ndspWaveBuf wb;
    bool        used = false;
};
Voice s_voices[MAX_SND_VOICES];

struct Copy { void* lin; s32 size; };
std::map<const void*, Copy> s_copies;

bool ndspUp()
{
    if (s_ndsp) return true;
    if (s_ndspFailed) return false;
    Result r = ndspInit();
    if (R_FAILED(r)) {
        /* no sdmc:/3ds/dspfirm.cdc (dumped with DSP1): no sound */
        SYS_Report("[Audio3ds] ndspInit failed: %08lx (sdmc:/3ds/dspfirm.cdc?)\n", (unsigned long)r);
        s_ndspFailed = true;
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    s_ndsp = true;
    return true;
}

void setMix(int ch, s32 l, s32 r)
{
    float mix[12] = {};
    mix[0] = l / 255.0f;
    mix[1] = r / 255.0f;
    ndspChnSetMix(ch, mix);
}

} // namespace

extern "C" {

void ASND_Init(void) { ndspUp(); }

void ASND_End(void)
{
    if (!s_ndsp) return;
    for (int v = 0; v < MAX_SND_VOICES; ++v) ASND_StopVoice(v);
}

void ASND_Pause(s32 paused)
{
    s_paused = paused != 0;
    if (!s_ndsp) return;
    for (int v = 0; v < MAX_SND_VOICES; ++v) ndspChnSetPaused(v, s_paused);
}

s32 ASND_Is_Paused(void) { return s_paused; }

static s32 setVoice(s32 voice, s32 format, s32 pitch, void* snd, s32 size, s32 vl, s32 vr, bool loop)
{
    if (voice < 0 || voice >= MAX_SND_VOICES || !snd || size <= 0) return SND_INVALID;
    if (!ndspUp()) return SND_INVALID;
    void* lin = snd;
    {   /* the DSP reads linear memory: a copy there, made once */
        auto it = s_copies.find(snd);
        if (it != s_copies.end() && it->second.size != size) {
            linearFree(it->second.lin);
            s_copies.erase(it);
            it = s_copies.end();
        }
        if (it == s_copies.end()) {
            void* c = linearAlloc(size);
            if (!c) return SND_INVALID;
            memcpy(c, snd, size);
            it = s_copies.emplace(snd, Copy{ c, size }).first;
        }
        lin = it->second.lin;
    }
    DSP_FlushDataCache(lin, size);
    ndspChnReset(voice);
    const bool stereo = format == VOICE_STEREO_16BIT || format == VOICE_STEREO_8BIT;
    const bool bits16 = format == VOICE_MONO_16BIT || format == VOICE_STEREO_16BIT;
    ndspChnSetInterp(voice, NDSP_INTERP_LINEAR);
    ndspChnSetRate(voice, (float)pitch);
    ndspChnSetFormat(voice, (stereo ? NDSP_CHANNELS(2) : NDSP_CHANNELS(1)) |
                            (bits16 ? NDSP_ENCODING(NDSP_ENCODING_PCM16) : NDSP_ENCODING(NDSP_ENCODING_PCM8)));
    setMix(voice, vl, vr);
    Voice& v = s_voices[voice];
    memset(&v.wb, 0, sizeof(v.wb));
    v.wb.data_vaddr = lin;
    v.wb.nsamples = size / ((stereo ? 2 : 1) * (bits16 ? 2 : 1));
    v.wb.looping = loop;
    v.used = true;
    ndspChnWaveBufAdd(voice, &v.wb);
    if (s_paused) ndspChnSetPaused(voice, true);
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
    if (s_ndsp) ndspChnWaveBufClear(voice);
    s_voices[voice].used = false;
    return SND_OK;
}

s32 ASND_PauseVoice(s32 voice, s32 pause)
{
    if (voice < 0 || voice >= MAX_SND_VOICES || !s_ndsp) return SND_INVALID;
    ndspChnSetPaused(voice, pause != 0);
    return SND_OK;
}

s32 ASND_StatusVoice(s32 voice)
{
    if (voice < 0 || voice >= MAX_SND_VOICES) return SND_INVALID;
    Voice& v = s_voices[voice];
    if (!v.used || !s_ndsp) return SND_UNUSED;
    if (v.wb.status == NDSP_WBUF_DONE) { v.used = false; return SND_UNUSED; }
    return SND_WORKING;
}

s32 ASND_ChangeVolumeVoice(s32 voice, s32 vl, s32 vr)
{
    if (voice < 0 || voice >= MAX_SND_VOICES || !s_ndsp) return SND_INVALID;
    setMix(voice, vl, vr);
    return SND_OK;
}

s32 ASND_GetFirstUnusedVoice(void)
{
    for (int v = 1; v < MAX_SND_VOICES; ++v)
        if (ASND_StatusVoice(v) == SND_UNUSED) return v;
    return SND_INVALID;
}

} // extern "C"

/* ---- MP3Player ------------------------------------------------------------ */

namespace {

const int MP3_CH = 16;
const int MP3_BUFS = 4;
const int MP3_SAMPLES = 4096;           /* stereo frames per buffer */

Thread         s_mp3Thread = nullptr;
volatile bool  s_mp3Stop = false;
volatile bool  s_mp3Playing = false;
const u8*      s_mp3Data = nullptr;
s32            s_mp3Len = 0;
float          s_mp3Vol = 1.0f;
ndspWaveBuf    s_mp3Wb[MP3_BUFS];
s16*           s_mp3Pcm = nullptr;

inline s16 toS16(mad_fixed_t s)
{
    s += 1L << (MAD_F_FRACBITS - 16);
    if (s >= MAD_F_ONE) s = MAD_F_ONE - 1;
    else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
    return (s16)(s >> (MAD_F_FRACBITS + 1 - 16));
}

void mp3Main(void*)
{
    mad_stream stream;
    mad_frame frame;
    mad_synth synth;
    mad_stream_init(&stream);
    mad_frame_init(&frame);
    mad_synth_init(&synth);
    mad_stream_buffer(&stream, s_mp3Data, s_mp3Len);
    bool rateSet = false;
    unsigned pos = 0, have = 0;   /* samples of the frame decoded last */
    int buf = 0, fill = 0;
    bool ended = false;
    while (!s_mp3Stop) {
        ndspWaveBuf& wb = s_mp3Wb[buf];
        if (wb.status != NDSP_WBUF_FREE && wb.status != NDSP_WBUF_DONE) { svcSleepThread(5 * 1000 * 1000); continue; }
        s16* out = s_mp3Pcm + buf * MP3_SAMPLES * 2;
        while (fill < MP3_SAMPLES && !ended) {
            if (pos >= have) {
                if (mad_frame_decode(&frame, &stream) == -1) {
                    if (MAD_RECOVERABLE(stream.error)) continue;
                    ended = true;
                    break;
                }
                mad_synth_frame(&synth, &frame);
                pos = 0;
                have = synth.pcm.length;
                if (!rateSet) {
                    ndspChnSetRate(MP3_CH, (float)synth.pcm.samplerate);
                    rateSet = true;
                }
            }
            const mad_pcm& pcm = synth.pcm;
            while (fill < MP3_SAMPLES && pos < have) {
                s16 l = toS16(pcm.samples[0][pos]);
                s16 r = pcm.channels == 2 ? toS16(pcm.samples[1][pos]) : l;
                out[fill * 2] = l;
                out[fill * 2 + 1] = r;
                ++fill;
                ++pos;
            }
        }
        if (fill > 0) {
            DSP_FlushDataCache(out, fill * 4);
            wb.data_vaddr = out;
            wb.nsamples = fill;
            ndspChnWaveBufAdd(MP3_CH, &wb);
            buf = (buf + 1) % MP3_BUFS;
            fill = 0;
        }
        if (ended) {
            /* wait for what was queued to be heard */
            bool busy = true;
            while (busy && !s_mp3Stop) {
                busy = false;
                for (auto& b : s_mp3Wb) busy |= b.status == NDSP_WBUF_QUEUED || b.status == NDSP_WBUF_PLAYING;
                if (busy) svcSleepThread(10 * 1000 * 1000);
            }
            break;
        }
    }
    mad_synth_finish(&synth);
    mad_frame_finish(&frame);
    mad_stream_finish(&stream);
    s_mp3Playing = false;
}

} // namespace

extern "C" {

void MP3Player_Init(void) { ndspUp(); }

void MP3Player_Stop(void)
{
    if (s_mp3Thread) {
        s_mp3Stop = true;
        threadJoin(s_mp3Thread, U64_MAX);
        threadFree(s_mp3Thread);
        s_mp3Thread = nullptr;
    }
    if (s_ndsp) ndspChnWaveBufClear(MP3_CH);
    s_mp3Playing = false;
}

s32 MP3Player_PlayBuffer(const void* buffer, s32 len, void (*filter)(void*, s32*))
{
    (void)filter;
    MP3Player_Stop();
    if (!ndspUp() || !buffer || len <= 0) return -1;
    if (!s_mp3Pcm) s_mp3Pcm = (s16*)linearAlloc(MP3_BUFS * MP3_SAMPLES * 4);
    if (!s_mp3Pcm) return -1;
    memset(s_mp3Wb, 0, sizeof(s_mp3Wb));
    ndspChnReset(MP3_CH);
    ndspChnSetInterp(MP3_CH, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(MP3_CH, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(MP3_CH, 44100.0f);
    float mix[12] = {};
    mix[0] = mix[1] = s_mp3Vol;
    ndspChnSetMix(MP3_CH, mix);
    s_mp3Data = (const u8*)buffer;
    s_mp3Len = len;
    s_mp3Stop = false;
    s_mp3Playing = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_mp3Thread = threadCreate(mp3Main, nullptr, 64 * 1024, prio > 0x18 ? prio - 1 : prio, -2, false);
    if (!s_mp3Thread) { s_mp3Playing = false; return -1; }
    return 0;
}

bool MP3Player_IsPlaying(void) { return s_mp3Playing; }

void MP3Player_Volume(u32 volume)
{
    s_mp3Vol = volume / 255.0f;
    if (!s_ndsp) return;
    float mix[12] = {};
    mix[0] = mix[1] = s_mp3Vol;
    ndspChnSetMix(MP3_CH, mix);
}

} // extern "C"
