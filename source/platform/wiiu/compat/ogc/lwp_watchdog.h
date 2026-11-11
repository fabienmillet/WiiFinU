#pragma once
/* libogc's time base on the Wii U: ticks are microseconds since start */
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
u64 gettime(void);
static inline u32 ticks_to_millisecs(u64 t) { return (u32)(t / 1000); }
static inline u64 ticks_to_microsecs(u64 t) { return t; }
static inline u64 ticks_to_nanosecs(u64 t) { return t * 1000; }
static inline u64 millisecs_to_ticks(u64 ms) { return ms * 1000; }
static inline u64 microsecs_to_ticks(u64 us) { return us; }
static inline u32 diff_msec(u64 a, u64 b) { return (u32)((b - a) / 1000); }
static inline u64 diff_usec(u64 a, u64 b) { return b - a; }
static inline u64 diff_ticks(u64 a, u64 b) { return b - a; }
#define TB_TIMER_CLOCK 1000
#ifdef __cplusplus
}
#endif
