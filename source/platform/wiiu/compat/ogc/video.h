#pragma once
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { u32 viTVMode; u16 fbWidth, efbHeight, xfbHeight, viXOrigin, viYOrigin, viWidth, viHeight; } GXRModeObj;
extern GXRModeObj* rmode;
#define VI_PROGRESSIVE 2
void VIDEO_WaitVSync(void);
static inline void VIDEO_Init(void) {}
static inline GXRModeObj* VIDEO_GetPreferredMode(GXRModeObj* m) { (void)m; return rmode; }
static inline void VIDEO_Configure(GXRModeObj* m) { (void)m; }
static inline void VIDEO_SetNextFramebuffer(void* fb) { (void)fb; }
static inline void VIDEO_SetBlack(bool b) { (void)b; }
static inline void VIDEO_Flush(void) {}
static inline void VIDEO_ClearFrameBuffer(GXRModeObj* m, void* fb, u32 c) { (void)m; (void)fb; (void)c; }
static inline void* SYS_AllocateFramebuffer(GXRModeObj* m) { (void)m; return 0; }
typedef void (*VIRetraceCallback)(u32 retraceCnt);
static inline VIRetraceCallback VIDEO_SetPostRetraceCallback(VIRetraceCallback cb) { (void)cb; return 0; }
#ifdef __cplusplus
}
#endif
