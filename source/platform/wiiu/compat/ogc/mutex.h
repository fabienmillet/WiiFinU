#pragma once
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef u32 mutex_t;
#define LWP_MUTEX_NULL 0xffffffff
s32 LWP_MutexInit(mutex_t* mutex, bool use_recursive);
s32 LWP_MutexDestroy(mutex_t mutex);
s32 LWP_MutexLock(mutex_t mutex);
s32 LWP_MutexTryLock(mutex_t mutex);
s32 LWP_MutexUnlock(mutex_t mutex);
#ifdef __cplusplus
}
#endif
