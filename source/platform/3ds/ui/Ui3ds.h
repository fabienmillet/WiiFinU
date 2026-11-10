#pragma once
/* -----------------------------------------------------------------------
 * Ui3ds — the 3DS's own screens: drawn 1:1 in the screens' pixels (the
 * bottom one 320x240, the top one 400x240) instead of the Wii's 640x480
 * frame at half size, with the theme's colours (Ui::pal) and Ui's shapes.
 * Text sizes are pixels; touch targets are at least 28 pixels.
 * ----------------------------------------------------------------------- */
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <string>
#include <vector>

#ifndef APP_NAME
#define APP_NAME "3DSFin"   /* the 3DS version's name: to be chosen (Discord) */
#endif

namespace Ui3ds {

const int BW = 320, BH = 240;   /* bottom screen */
const int TW = 400, TH = 240;   /* top screen */

/* Draw in the bottom screen's pixels (from now on), or the top one's
 * (inside the top screen's drawer); wiiSpace(): back to the 640x480 frame
 * the Wii's screens draw in. */
void bottomSpace();
void topSpace();
void wiiSpace();

/* The touch: where the stylus is (bottom screen pixels), whether it
 * touches, and a tap (lifted where it touched) this frame. */
struct Touch {
    bool  down = false;      /* the stylus on the screen */
    float x = 0, y = 0;
    bool  tap = false;       /* lifted without dragging: a press there */
    bool  pressed = false;   /* touched this frame */
    float dx = 0, dy = 0;    /* moved since the previous frame */
    float startX = 0, startY = 0;
    bool  dragging = false;  /* moved far enough to be a drag */
};
/* Reads it from the Wii Remote pointer Input gives (call once a frame,
 * after Input::update / readIR). */
void readTouch(const ir_t& ir, Touch& t);

/* ---- drawing ---------------------------------------------------------- */
void rect(float x, float y, float w, float h, u32 color);
void gradient(float x, float y, float w, float h, u32 top, u32 bottom);
void background(int w, int h);
/* a texture covering x,y,w,h (cropped), rounded corners r */
void image(GRRLIB_texImg* tex, float x, float y, float w, float h, float r = 0, u32 tint = 0xFFFFFFFF);
/* text; returns its width.  Long text is cut with an ellipsis to maxW */
float text(float x, float y, const std::string& s, int size, u32 color, float maxW = 0);
float textW(const std::string& s, int size);
void  textC(float cx, float y, const std::string& s, int size, u32 color, float maxW = 0);
void  textR(float xr, float y, const std::string& s, int size, u32 color);
/* wrapped into lines of maxW, at most maxLines (the last cut with an
 * ellipsis); returns the height used */
float paragraph(float x, float y, float maxW, const std::string& s, int size, u32 color,
                int maxLines, float lineH = 0);
std::string ellipsize(const std::string& s, int size, float maxW);

/* icons, drawn in a box of size s centred on cx, cy */
enum class Icon { Back, Search, Gear, Play, Pause, Prev, Next, Rewind, Forward, Heart, Check,
                  Home, Person, Audio, Subtitles, Shuffle, Grid, Close };
void icon(Icon i, float cx, float cy, float s, u32 color);

/* A round-cornered button with an icon and/or a label; focus 0..1 (the
 * D-pad's selection) lights it.  hit(): the tap is in it. */
void button(float x, float y, float w, float h, const std::string& label, int size,
            float focus, bool primary = false, Icon ic = Icon::Close, bool withIcon = false);
inline bool hit(const Touch& t, float x, float y, float w, float h)
{ return t.tap && t.x >= x && t.x < x + w && t.y >= y && t.y < y + h; }

/* The bottom screen's bar: back arrow (when canBack), title, then the
 * icons given on the right.  Returns the index of the icon tapped (-1 the
 * back arrow, -2 nothing). */
int topBar(const Touch& t, const std::string& title, bool canBack, const Icon* icons, int n,
           int focusIcon = -1);
const int BAR_H = 28;

/* A thin progress bar (watched part of an item) */
void progress(float x, float y, float w, float frac);

/* A small spinner (loading) */
void spinner(float cx, float cy, float r);

/* ---- scrolling ---------------------------------------------------------- */
/* Follows the stylus dragging a list (vertical or horizontal), coasting
 * when it lets go; the D-pad's selection is kept in view with show(). */
struct Scroller {
    float pos = 0, vel = 0, target = -1;
    float maxPos = 0;           /* content size - viewport size */
    bool  horizontal = false;
    bool  grabbed = false;
    /* t: the touch; inside: the drag started in this list's area */
    void update(const Touch& t, bool inside);
    /* make [a, b) visible in a viewport of size view */
    void show(float a, float b, float view);
};

/* ---- the top screen ------------------------------------------------------ */
/* What the top screen shows while browsing: the focused item */
struct TopInfo {
    std::string title, line1, line2, overview;
    GRRLIB_texImg* backdrop = nullptr;   /* wide picture, else ... */
    GRRLIB_texImg* poster = nullptr;     /* ... the poster at the side */
    float progress = -1;                 /* watched part, -1 none */
    bool  square = false;                /* the poster is a cover (music) */
};
/* drawn by Platform3ds's top drawer while set (nullptr: the logo) */
void setTopInfo(const TopInfo* info);
void drawTopInfo();
bool hasTopInfo();

/* the app's name as a logo (text: the name is still to be chosen) */
void logo(float cx, float cy, int size);

}
