#pragma once
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
void SYS_Report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define SYS_RESTART      0
#define SYS_HOTRESET     1
#define SYS_SHUTDOWN     2
#define SYS_RETURNTOMENU 3
#define SYS_POWEROFF     4
#define SYS_POWEROFF_STANDBY 5
#define SYS_POWEROFF_IDLE    6
#define SYS_STDIO_REPORT 0
void SYS_ResetSystem(s32 reset, u32 type, s32 force);
typedef void (*powercallback)(void);
typedef void (*resetcallback)(u32 irq, void* ctx);
static inline powercallback SYS_SetPowerCallback(powercallback cb) { (void)cb; return 0; }
static inline resetcallback SYS_SetResetCallback(resetcallback cb) { (void)cb; return 0; }
u32 SYS_GetArena1Size(void);
u32 SYS_GetArena2Size(void);
static inline void* SYS_GetArena1Lo(void) { return 0; }
static inline void* SYS_GetArena2Lo(void) { return 0; }
static inline void* SYS_GetArena1Hi(void) { return 0; }
static inline void* SYS_GetArena2Hi(void) { return 0; }
static inline void SYS_SetTimeslice(u32 v) { (void)v; }
static inline void SYS_STDIO_Report(bool v) { (void)v; }
#define MEM_K0_TO_K1(x) (x)
#ifdef __cplusplus
}
#endif
