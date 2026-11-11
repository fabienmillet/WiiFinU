#pragma once
/* the SD card is fs:/vol/external01/ on the Wii U, mounted by wut */
static inline bool fatInitDefault(void) { return true; }
static inline bool fatMountSimple(const char* n, const void* io) { (void)n; (void)io; return true; }
