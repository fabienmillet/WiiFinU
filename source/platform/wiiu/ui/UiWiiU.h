#pragma once
/* -----------------------------------------------------------------------
 * UiWiiU — the Wii U's own browsing screens: drawn in the screens' own
 * spaces instead of the Wii's 640x480 frame: the GamePad's 854x480 pixels
 * and, on the TV, 1280x720 units (a 1080p TV: 1.5 pixels a unit, glyphs
 * rasterised for it).  WiiFin's colours (Ui::pal) and shapes, with the
 * Wii U Menu's touches: a light pattern behind, rounded tiles in a frame.
 * Touch targets are at least 44 pixels.
 * ----------------------------------------------------------------------- */
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <string>
#include <vector>

namespace UiWiiU {

const int PW = 854, PH = 480;    /* the GamePad */
const int TW = 1280, TH = 720;   /* the TV */

/* Draw in the GamePad's pixels (from now on), or the TV's units;
 * wiiSpace(): back to the Wii's 16:9 frame (Ui::initScreen's). */
void padSpace();
void tvSpace();
void wiiSpace();
/* scissor in the current space's units (and off) */
void clip(float x, float y, float w, float h);
void clipOff();

/* The touch screen: where the finger is (GamePad pixels), whether it
 * touches, and a tap (lifted where it touched) this frame. */
struct Touch {
    bool  down = false;
    float x = 0, y = 0;
    bool  tap = false;
    bool  pressed = false;   /* touched this frame */
    float dx = 0, dy = 0;    /* moved since the previous frame */
    float startX = 0, startY = 0;
    bool  dragging = false;
};
void readTouch(Touch& t);

/* ---- drawing ---------------------------------------------------------- */
void rect(float x, float y, float w, float h, u32 color);
void gradient(float x, float y, float w, float h, u32 top, u32 bottom);
/* left to right */
void hgradient(float x, float y, float w, float h, u32 left, u32 right);
/* the theme's background with the Wii U's diagonal pattern */
void background(float w, float h);
/* a texture covering x,y,w,h (cropped), rounded corners r */
void image(GRRLIB_texImg* tex, float x, float y, float w, float h, float r = 0, u32 tint = 0xFFFFFFFF);
/* a Wii U tile: its frame and shadow, lit by focus (0..1) */
void tileFrame(float x, float y, float w, float h, float r, float focus);
/* text; returns its width.  Long text is cut with an ellipsis to maxW */
float text(float x, float y, const std::string& s, int size, u32 color, float maxW = 0);
float textW(const std::string& s, int size);
void  textC(float cx, float y, const std::string& s, int size, u32 color, float maxW = 0);
void  textR(float xr, float y, const std::string& s, int size, u32 color);
/* text with a soft dark shadow (on pictures) */
float textS(float x, float y, const std::string& s, int size, u32 color, float maxW = 0);
/* wrapped into lines of maxW, at most maxLines (the last cut with an
 * ellipsis); returns the height used */
float paragraph(float x, float y, float maxW, const std::string& s, int size, u32 color,
                int maxLines, float lineH = 0);
std::string ellipsize(const std::string& s, int size, float maxW);

enum class Icon { Back, Search, Gear, Play, Pause, Prev, Next, Rewind, Forward, Heart, Check,
                  Home, Person, Audio, Subtitles, Shuffle, Grid, Close, Star, Music };
void icon(Icon i, float cx, float cy, float s, u32 color);

/* A rounded button with an icon and/or a label; focus 0..1 lights it */
void button(float x, float y, float w, float h, const std::string& label, int size,
            float focus, bool primary = false, Icon ic = Icon::Close, bool withIcon = false);
/* a round button with an icon (the bar's) */
void roundButton(float cx, float cy, float r, Icon ic, float focus, bool pressed = false);
inline bool hit(const Touch& t, float x, float y, float w, float h)
{ return t.tap && t.x >= x && t.x < x + w && t.y >= y && t.y < y + h; }

/* a thin progress bar (watched part) */
void progress(float x, float y, float w, float frac, float h = 4);
/* a small spinner (loading) */
void spinner(float cx, float cy, float r);
/* the WiiFin logo (its picture), height h, centred vertically on cy */
void logo(float x, float cy, float h);
/* hints at the bottom: "A Open"... (a button's Wii name, its label) */
struct Hint { const char* button; const char* label; };
void hints(float x, float y, const Hint* h, int n, int size, u32 color, bool rightAlign = false);
/* the clock, as the settings want it */
std::string clockText();

/* ---- scrolling ---------------------------------------------------------- */
/* Follows the finger dragging a list, coasting when it lets go; the
 * D-pad's selection is kept in view with show(). */
struct Scroller {
    float pos = 0, vel = 0, target = -1;
    float maxPos = 0;
    bool  horizontal = false;
    bool  grabbed = false;
    void update(const Touch& t, bool inside);
    void show(float a, float b, float view);
};

}
