#pragma once
#include "../gctypes.h"
static inline void DCFlushRange(void* p, u32 n) { (void)p; (void)n; }
static inline void DCInvalidateRange(void* p, u32 n) { (void)p; (void)n; }
static inline void DCStoreRange(void* p, u32 n) { (void)p; (void)n; }
static inline void ICInvalidateRange(void* p, u32 n) { (void)p; (void)n; }
