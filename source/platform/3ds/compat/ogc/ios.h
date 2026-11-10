#pragma once
#include "../gctypes.h"
static inline s32 IOS_GetVersion(void) { return 0; }
static inline s32 IOS_GetRevision(void) { return 0; }
#ifdef __cplusplus
extern "C" {
#endif
s32 ES_GetDeviceID(u32* id);
static inline s32 ES_GetTitleID(u64* tid) { *tid = 0; return -1; }
#ifdef __cplusplus
}
#endif
