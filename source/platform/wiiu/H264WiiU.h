#pragma once
/* -----------------------------------------------------------------------
 * H264WiiU — the Wii U's H.264 decoder (h264.rpl, the console's hardware)
 * for PlayerWiiU: HD pictures without the CPU.
 *
 * Access units go in (Annex B, one picture each, with their time); the
 * pictures come out in display order, NV12, in a pool of frame buffers
 * the decoder writes (round robin: enough of them that the pictures it
 * still refers to and the ones waiting to be shown are never written
 * over), each with a pair of textures on its memory for the GPU.
 * One decoder at a time (PlayerWiiU's thread).
 * ----------------------------------------------------------------------- */
#include <stdint.h>

struct TexWiiU;

namespace H264WiiU {

/* For a stream of that size (pixels) and profile (66, 77, 100); false:
 * not possible (the profile, no memory) */
bool open(int width, int height, int profile);
void close();
bool isOpen();

/* One access unit (Annex B) and its time.  false: the decoder refused it */
bool decode(const uint8_t* data, int size, double pts);
/* At the end of the stream: the pictures it still holds come out */
void drain();
/* After a seek: everything held is dropped */
void reset();

/* The next picture in display order: its frame's textures and time.
 * false: none yet. */
bool next(TexWiiU*& y, TexWiiU*& uv, double& pts);
/* the frames the player shows and holds (not decoded into) */
void inUse(TexWiiU* shown, TexWiiU* next);
/* pictures decoded so far */
unsigned decoded();

}
