#pragma once
/* libogc's ASND (16 voices) on the Wii U, mixed in software (AudioWiiU.cpp).
 * Samples are the CPU's s16 (little-endian here). */
#include "gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SND_OK 0
#define SND_INVALID -1
#define SND_ISNOTASONGVOICE -2
#define SND_BUSY 1
#define SND_UNUSED 0
#define SND_WORKING 1
#define SND_WAITING 2
#define MAX_SND_VOICES 16
#define MAX_VOLUME 255
#define VOICE_MONO_8BIT    0
#define VOICE_MONO_16BIT   1
#define VOICE_STEREO_8BIT  2
#define VOICE_STEREO_16BIT 3
#define VOICE_MONO_16BIT_BE   VOICE_MONO_16BIT
#define VOICE_STEREO_16BIT_BE VOICE_STEREO_16BIT
#define VOICE_MONO_16BIT_LE   VOICE_MONO_16BIT
#define VOICE_STEREO_16BIT_LE VOICE_STEREO_16BIT
#define VOICE_FREQ32KHZ 32000
#define VOICE_FREQ48KHZ 48000
typedef void (*ASNDVoiceCallback)(s32 voice);
void ASND_Init(void);
void ASND_End(void);
void ASND_Pause(s32 paused);
s32  ASND_Is_Paused(void);
s32  ASND_SetVoice(s32 voice, s32 format, s32 pitch, s32 delay, void* snd, s32 size_snd,
                   s32 volume_l, s32 volume_r, ASNDVoiceCallback callback);
s32  ASND_SetInfiniteVoice(s32 voice, s32 format, s32 pitch, s32 delay, void* snd, s32 size_snd,
                           s32 volume_l, s32 volume_r);
/* a voice set with a callback: its next block (SND_BUSY: one waits already) */
s32  ASND_AddVoice(s32 voice, void* snd, s32 size_snd);
s32  ASND_StopVoice(s32 voice);
s32  ASND_PauseVoice(s32 voice, s32 pause);
s32  ASND_StatusVoice(s32 voice);
s32  ASND_ChangeVolumeVoice(s32 voice, s32 volume_l, s32 volume_r);
s32  ASND_GetFirstUnusedVoice(void);
#ifdef __cplusplus
}
#endif
