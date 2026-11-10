#pragma once
#include "../gctypes.h"
#define AUDIO_BPS 4
static inline u32 AUDIO_GetDMABytesLeft(void) { return 0; }
static inline u32 AUDIO_GetDMAStartAddr(void) { return 0; }
static inline u32 AUDIO_GetDMALength(void) { return 0; }
static inline u32 AUDIO_GetDMAEnableFlag(void) { return 0; }
