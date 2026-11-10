#pragma once
/* -----------------------------------------------------------------------
 * Gfx3ds — WiiFin's picture on the 3DS's two screens.
 *
 * The Wii's 640x480 frame is the bottom screen (320x240, the touch
 * screen: the Wii Remote's pointer is the stylus), at half size: views,
 * GRRLIB and the GX calls of Ui keep their coordinates.  The top screen
 * (400x240) is drawn by its drawer (setTopDrawer) at each GRRLIB_Render,
 * in an 800x480 space at half size the same way: the video, the poster
 * of what is selected, the logo...
 * ----------------------------------------------------------------------- */
#include <grrlib.h>

/* A texture on the 3DS's GPU (GRRLIB_texImg::data on the 3DS) */
struct Tex3ds;

namespace Gfx3ds {

void init();
void exit();

/* Pixel formats of make() */
enum class Fmt { RGBA8, RGB565, LA8 };

/* A texture of w x h pixels from rows of pixels (stride: bytes a row):
 * RGBA8 4 bytes R,G,B,A; RGB565 2 bytes (a u16); LA8 2 bytes alpha then
 * luminance.  Bigger than 1024 pixels: shrunk (every other pixel...).
 * nullptr: no memory. */
Tex3ds* make(Fmt fmt, int w, int h, const void* pixels, int stride, bool vram = true);
/* Same pixels again into an existing texture of that size and format
 * (the video's frames) */
void    update(Tex3ds* t, const void* pixels, int stride);
void    destroy(Tex3ds* t);
/* Its texels (linear memory, the GPU's 8x8 tiles) and the texture's
 * power-of-2 size: for hardware writing into it (Y2R) */
void*   pixels(Tex3ds* t, int& pw, int& ph);
int     width(const Tex3ds* t);
int     height(const Tex3ds* t);
/* A GRRLIB texture around one (GRRLIB_FreeTexture frees both) */
GRRLIB_texImg* wrap(Tex3ds* t);

/* What the top screen shows; drawn in an 800x480 space (GX and Ui
 * calls, like the bottom's 640x480).  nullptr: the default one (WiiFin's
 * logo).  Called in GRRLIB_Render. */
void setTopDrawer(void (*draw)());
void defaultTop();
/* While true, the bottom screen's backlight is off (watching a video) */
void setBottomOff(bool off);

/* Before a system applet (the keyboard): the frame being drawn ends (the
 * next one begins at the next drawing) */
void endFrameForApplet();

/* Frames shown so far (each GRRLIB_Render) */
unsigned frames();

}
