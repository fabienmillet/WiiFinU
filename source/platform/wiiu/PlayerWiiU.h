#pragma once
/* The Wii U's player (PlayerWiiU.cpp): WiiPlayer.h's interface (what
 * MPlayer does on the Wii) with FFmpeg, the sound in AudioWiiU's stream,
 * the picture drawn by VideoSurface in the main frame. */
namespace PlayerWiiU {
/* A video is playing (its pictures are drawn) */
bool videoActive();
/* paused (the HOME menu), not toggled */
void pause();
/* The newest picture covering (x, y, w, h) of the drawing space (cut to
 * it): the GamePad's remote.  false: none. */
bool drawPreview(float x, float y, float w, float h);

/* HD video on the TV (Settings > HD Video, saved as wiiu_hd): 480p,
 * 720p or 1080p.  Videos come as H.264 (the console's own decoder) at that
 * size on the TV, at 480p on the GamePad; once that decoder refused a
 * stream, as MPEG-4 at 480p (FFmpeg, as the Wii). */
enum Hd { HD_OFF = 0, HD_720 = 1, HD_1080 = 2 };
void setHd(int hd);
int  hd();
/* what playback asks for now: the size (the setting on the TV, 480p on
 * the GamePad), and whether as H.264 */
int  hdNow();
bool h264Now();
}
