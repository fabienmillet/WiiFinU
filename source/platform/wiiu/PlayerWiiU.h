#pragma once
/* The Wii U's player (PlayerWiiU.cpp): WiiPlayer.h's interface (what
 * MPlayer does on the Wii) with FFmpeg, the sound in AudioWiiU's stream,
 * the picture drawn by VideoSurface in the main frame. */
namespace PlayerWiiU {
/* A video is playing (its pictures are drawn) */
bool videoActive();
/* The newest picture covering (x, y, w, h) of the drawing space (cut to
 * it): the GamePad's remote.  false: none. */
bool drawPreview(float x, float y, float w, float h);

/* HD video on the TV (Settings > HD Video, saved as wiiu_hd): H.264
 * converted to 720p or 1080p by the server, decoded by the console's own
 * decoder.  On the GamePad (or once that decoder refused a stream):
 * 480p, MPEG-4. */
enum Hd { HD_OFF = 0, HD_720 = 1, HD_1080 = 2 };
void setHd(int hd);
int  hd();
/* what playback asks for now: the setting on the TV, else HD_OFF */
int  hdNow();
}
