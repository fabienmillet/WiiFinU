#pragma once
/* libogc threads on the Wii U (coreinit threads; see OgcWiiU.cpp) */
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef u32 lwp_t;
typedef u32 lwpq_t;
#define LWP_THREAD_NULL 0xffffffff
#define LWP_TQUEUE_NULL 0xffffffff
#define LWP_PRIO_IDLE    0
#define LWP_PRIO_HIGHEST 127
s32  LWP_CreateThread(lwp_t* thethread, void* (*entry)(void*), void* arg, void* stackbase,
                      u32 stack_size, u8 prio);
s32  LWP_JoinThread(lwp_t thethread, void** value_ptr);
lwp_t LWP_GetSelf(void);
void LWP_SetThreadPriority(lwp_t thethread, u32 prio);
void LWP_YieldThread(void);
s32  LWP_SuspendThread(lwp_t thethread);
s32  LWP_ResumeThread(lwp_t thethread);
#ifdef __cplusplus
}
#endif
