#pragma once
/* the SD card is sdmc:/ on the 3DS, mounted by libctru */
static inline bool fatInitDefault(void) { return true; }
static inline bool fatMountSimple(const char* n, const void* io) { (void)n; (void)io; return true; }
