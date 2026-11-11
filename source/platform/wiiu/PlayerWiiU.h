#pragma once
/* The Wii U's player (PlayerWiiU.cpp): WiiPlayer.h's interface (what
 * MPlayer does on the Wii) with FFmpeg, the sound in AudioWiiU's stream,
 * the picture drawn by VideoSurface in the main frame. */
namespace PlayerWiiU {
/* A video is playing (its pictures are drawn) */
bool videoActive();
}
