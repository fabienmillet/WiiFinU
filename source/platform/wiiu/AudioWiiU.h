#pragma once
/* -----------------------------------------------------------------------
 * AudioWiiU — the Wii U's sound: one output (SDL's, on the console's AX)
 * at 48 kHz stereo, where WiiFin's sounds are mixed in software: ASND's
 * 16 voices (the menus' sounds), MP3Player (the menus' music, libmad) and
 * the stream of the video or music playing (PlayerWiiU).
 * ----------------------------------------------------------------------- */
#include <stdint.h>

namespace AudioWiiU {

const int RATE = 48000;

/* Starts the output (once); at exit, closes it */
bool init();
void shutdown();

/* The player's stream: 48 kHz stereo s16, pushed with the time of its
 * first sample; up to `ahead` seconds held (push waits beyond, false:
 * `stop` became true meanwhile) */
void streamStart(double ahead);
void streamStop();
bool streamPush(const int16_t* pcm, int frames, double pts, volatile bool* stop);
/* frames held, not heard yet */
int  streamQueued();
/* the time heard now (s), < 0: nothing heard yet (or no output) */
double streamClock();
void streamFlush();
void streamPause(bool paused);
void streamVolume(float v);      /* 0..1 */

/* The last samples heard (mono, -1..1), the newest last: the music
 * player's bars */
int  recent(float* out, int n);

}
