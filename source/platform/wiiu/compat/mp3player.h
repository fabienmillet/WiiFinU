#pragma once
/* libogc's MP3Player (one MP3 in memory, played once) on the Wii U */
#include "gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
void MP3Player_Init(void);
s32  MP3Player_PlayBuffer(const void* buffer, s32 len, void (*filterfunc)(void*, s32*));
void MP3Player_Stop(void);
bool MP3Player_IsPlaying(void);
void MP3Player_Volume(u32 volume);
#ifdef __cplusplus
}
#endif
