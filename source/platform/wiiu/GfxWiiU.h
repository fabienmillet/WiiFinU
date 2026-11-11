#pragma once
/* -----------------------------------------------------------------------
 * GfxWiiU — WiiFin's picture on the Wii U's two screens (GX2).
 *
 * WiiFin draws its frames as on the Wii: GX's immediate mode and GRRLIB
 * in the Wii's 640x480 frame, which Ui widens to a 16:9 space of 854x480
 * drawing units (Ui::initScreen).  Each frame is recorded, then drawn at
 * GRRLIB_Render on the screens of the mode chosen:
 *   - Mirror: WiiFin on the GamePad (854x480, 1:1) and the same on the TV;
 *   - Remote: WiiFin on the TV (1280x720: 1.5 a unit), and on the GamePad
 *     what its drawer (setRemoteDrawer) draws in an 854x480 space: the
 *     remote control (PlatformWiiU).
 * ----------------------------------------------------------------------- */
#include <grrlib.h>

/* A texture on the Wii U's GPU (GRRLIB_texImg::data on the Wii U) */
struct TexWiiU;

namespace GfxWiiU {

void init();
void exit();

/* Pixel formats of make(): RGBA8 4 bytes R,G,B,A; LA8 2 bytes luminance
 * then alpha; R8 one byte (the video's Y), RG8 two (its interleaved U, V) */
enum class Fmt { RGBA8, LA8, R8, RG8 };

/* A texture of w x h pixels from rows of pixels (stride: bytes a row;
 * pixels nullptr: cleared).  Bigger than 2048 pixels: shrunk.  Any thread.
 * nullptr: no memory. */
TexWiiU* make(Fmt fmt, int w, int h, const void* pixels, int stride);
/* A texture on memory of someone else's (the H.264 decoder's frames):
 * pitchBytes a row; ownsImage: freed with it (when the GPU is done) */
TexWiiU* wrapMemory(Fmt fmt, int w, int h, void* image, int pitchBytes, bool ownsImage);
/* Same pixels again into an existing texture of that size and format */
void     update(TexWiiU* t, const void* pixels, int stride);
/* Its texels (rows of pitch() texels, in the format's bytes) to write
 * into directly; then flush() before it is drawn */
void*    pixels(TexWiiU* t);
int      pitch(const TexWiiU* t);
void     flush(TexWiiU* t);
/* Written by other hardware (the H.264 decoder): the GPU's texture cache
 * forgets it before the next frame is drawn */
void     touched(TexWiiU* t);
/* Freed when the GPU no longer draws it (the next frame) */
void     destroy(TexWiiU* t);
int      width(const TexWiiU* t);
int      height(const TexWiiU* t);
/* A GRRLIB texture around one (GRRLIB_FreeTexture frees both) */
GRRLIB_texImg* wrap(TexWiiU* t);

/* The screens */
enum class Mode { Mirror, Remote };
void setMode(Mode m);
Mode mode();
/* The GamePad's drawer in Remote mode (an 854x480 space; GX and Ui calls,
 * glyphs rasterised 1:1).  Called in GRRLIB_Render. */
void setRemoteDrawer(void (*draw)());
/* Output pixels per drawing unit of the main frame: 1 on the GamePad,
 * 1.5 on a 720p TV (Ui's glyphs are rasterised for it) */
float mainScale();
float mainScaleY();
/* The main frame's drawing space (Ui::initScreen's) and the TV's size */
void tvSize(int& w, int& h);

/* The video's picture: NV12 (y: R8, uv: RG8 at half size) in the main
 * frame at (x, y, w, h) drawing units (GX's matrices as for any drawing);
 * bt709: HD's colours, else SD's (BT.601); limited range */
void drawNV12(TexWiiU* y, TexWiiU* uv, float x, float y0, float w, float h, bool bt709);

/* The pointer's texture (App.cpp), not drawn while the pointer is a
 * finger on the GamePad (hideCursor, InputWiiU) */
void setCursorTexture(GRRLIB_texImg* tex);
void hideCursor(bool hide);

/* Closed from the HOME menu: nothing is drawn any more */
bool exiting();

/* Frames shown so far (each GRRLIB_Render) */
unsigned frames();
/* The system's HOME menu / the app sent to the background: called when it
 * goes (true) and comes back (false) */
void setForegroundHook(void (*hook)(bool leaving));

}
