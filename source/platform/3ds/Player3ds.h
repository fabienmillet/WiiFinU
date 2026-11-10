#pragma once
/* The 3DS's player (Player3ds.cpp): WiiPlayer.h's interface (what MPlayer
 * does on the Wii) with FFmpeg, the sound on the DSP, the picture on the
 * top screen. */
namespace Player3ds {
/* Draw the video's current picture on the top screen (its 800x480 space),
 * fitted or filled (VideoSurface's zoom).  false: no picture (none yet,
 * or no video playing). */
bool drawTop();
/* Drawn over the picture (the top screen's 800x480 space) this frame:
 * the subtitles.  Asked again every frame. */
void setTopOverlay(void (*draw)(void* ctx), void* ctx);
/* A video is playing (its pictures go to the top screen) */
bool videoActive();
}
