/* -----------------------------------------------------------------------
 * PlatformWiiU — the Wii U's start, its screens, and the GamePad as a
 * remote control while WiiFin is on the TV (see PlatformWiiU.h).
 *
 * The remote (an 854x480 space, the GamePad's pixels): on the right the
 * arrows and A in their middle, L, B, R (the pages) and + (WiiFin's
 * menu) under them; on the left a touch pad that moves a pointer on the
 * TV (a tap: A there), or, while a video plays, its progress (a tap
 * there seeks) and big buttons to go back, pause, go on.  At the bottom,
 * the buttons the TV's screen offers (its hints: "A Open", "Y Search"...)
 * to tap.  At the top: WiiFin brought to the GamePad.  The GamePad's own
 * buttons work as anywhere.
 * ----------------------------------------------------------------------- */
#include "PlatformWiiU.h"
#include "GfxWiiU.h"
#include "InputWiiU.h"
#include "PlayerWiiU.h"
#include "AudioWiiU.h"
#include "SwkbdWiiU.h"
#include "../../ui/Ui.h"
#include "../../core/Lang.h"
#include "../../core/Text.h"
#include "../../player/WiiPlayer.h"
#include "../../jellyfin/RemoteControl.h"

#include <avm/tv.h>
#include <coreinit/energysaver.h>
#include <tve/tve.h>
#include <coreinit/time.h>
#include <ogc/system.h>
#include <ogc/pad.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>

namespace {

PlatformWiiU::Screens s_screens = PlatformWiiU::Screens::Auto;
std::string s_playTitle, s_playSub;
float s_playPos = 0, s_playDur = 0;

uint64_t nowMs() { return (uint64_t)OSTicksToMilliseconds(OSGetSystemTime()); }

/* ---- the remote's controls ---------------------------------------------- */

enum Ctl { C_NONE, C_UP, C_DOWN, C_LEFT, C_RIGHT, C_A, C_B, C_L, C_R, C_MENU, C_PAD,
           C_BACK10, C_PLAY, C_FWD30, C_BAR, C_HERE, C_HINT0, C_COUNT = C_HINT0 + 8 };

struct Rect { float x, y, w, h; };
bool inside(const Rect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* the arrows' cross */
const float DX = 690, DY = 196, DS = 78;   /* centre, a button's size */
const Rect R_UP    = { DX - DS / 2, DY - DS * 1.5f - 6, DS, DS };
const Rect R_DOWN  = { DX - DS / 2, DY + DS * 0.5f + 6, DS, DS };
const Rect R_LEFT  = { DX - DS * 1.5f - 6, DY - DS / 2, DS, DS };
const Rect R_RIGHT = { DX + DS * 0.5f + 6, DY - DS / 2, DS, DS };
const Rect R_A     = { DX - DS / 2, DY - DS / 2, DS, DS };
const Rect R_L     = { 566, 342, 56, 52 };
const Rect R_B     = { 630, 342, 56, 52 };
const Rect R_R     = { 694, 342, 56, 52 };
const Rect R_MENU  = { 758, 342, 56, 52 };
/* the left side */
const Rect R_PAD   = { 24, 72, 500, 330 };
const Rect R_BAR   = { 44, 204, 460, 40 };
const Rect R_BACK10 = { 44, 290, 140, 92 };
const Rect R_PLAY  = { 204, 290, 140, 92 };
const Rect R_FWD30 = { 364, 290, 140, 92 };
/* the top: WiiFin to the GamePad */
const Rect R_HERE  = { 452, 14, 260, 40 };
/* the bottom: the TV screen's buttons */
const float HINT_Y = 418, HINT_H = 48, HINT_X0 = 24, HINT_X1 = 830;

bool playing();

/* the TV screen's hints as buttons: a button's Wii name, its label */
struct HintKey { const char* button; const char* label; };
HintKey s_hints[8];
int     s_nHints = 0;

void gatherHints()
{
    s_nHints = 0;
    for (const Ui::Hint& h : Ui::hintsShown()) {
        if (s_nHints >= 8) break;
        if (!h.button || !h.button[0] || strcmp(h.button, "UD") == 0 || strcmp(h.button, "LR") == 0) continue;
        if (strcmp(h.button, "-/+") == 0) {
            s_hints[s_nHints++] = { "-", h.label };
            if (s_nHints < 8) s_hints[s_nHints++] = { "+", h.label };
            continue;
        }
        if (!InputWiiU::buttonOf(h.button)) continue;
        s_hints[s_nHints++] = { h.button, h.label };
    }
    /* a video whose controls are hidden on the TV: the player's buttons */
    if (s_nHints == 0 && playing()) {
        static const HintKey PLAYER[] = { { "1", N_("Audio") }, { "2", N_("Subtitles") },
                                          { "+!", N_("Zoom") }, { "B", N_("Back") } };
        for (const HintKey& k : PLAYER) s_hints[s_nHints++] = k;
    }
}

Rect hintRect(int i)
{
    const float gap = 10, w = (HINT_X1 - HINT_X0 - gap * (s_nHints - 1)) / (s_nHints > 0 ? s_nHints : 1);
    return { HINT_X0 + i * (w + gap), HINT_Y, w, HINT_H };
}

Ctl     s_held = C_NONE;          /* touched, since the touch began on it */
uint64_t s_heldSince = 0, s_lastRepeat = 0;
uint32_t s_lastScan = 0;
/* the TV's pointer, moved by the pad */
float   s_ptrX = 320, s_ptrY = 240;
uint64_t s_ptrUsed = 0;
float   s_lastTX = 0, s_lastTY = 0;
float   s_barPick = -1;           /* a drag on the progress bar: 0..1 */

bool playing() { return PlayerWiiU::videoActive(); }

Ctl ctlAt(float x, float y)
{
    if (inside(R_UP, x, y)) return C_UP;
    if (inside(R_DOWN, x, y)) return C_DOWN;
    if (inside(R_LEFT, x, y)) return C_LEFT;
    if (inside(R_RIGHT, x, y)) return C_RIGHT;
    if (inside(R_A, x, y)) return C_A;
    if (inside(R_L, x, y)) return C_L;
    if (inside(R_B, x, y)) return C_B;
    if (inside(R_R, x, y)) return C_R;
    if (inside(R_HERE, x, y)) return C_HERE;
    if (inside(R_MENU, x, y)) return C_MENU;
    for (int i = 0; i < s_nHints; ++i)
        if (inside(hintRect(i), x, y)) return (Ctl)(C_HINT0 + i);
    if (playing()) {
        if (inside({ R_BAR.x, R_BAR.y - 16, R_BAR.w, R_BAR.h + 32 }, x, y)) return C_BAR;
        if (inside(R_BACK10, x, y)) return C_BACK10;
        if (inside(R_PLAY, x, y)) return C_PLAY;
        if (inside(R_FWD30, x, y)) return C_FWD30;
        return C_NONE;
    }
    if (inside(R_PAD, x, y)) return C_PAD;
    return C_NONE;
}

uint16_t padBits(Ctl c)
{
    switch (c) {
        case C_UP: return PAD_BUTTON_UP;
        case C_DOWN: return PAD_BUTTON_DOWN;
        case C_LEFT: return PAD_BUTTON_LEFT;
        case C_RIGHT: return PAD_BUTTON_RIGHT;
        case C_A: return PAD_BUTTON_A;
        case C_B: return PAD_BUTTON_B;
        case C_L: return PAD_TRIGGER_L;
        case C_R: return PAD_TRIGGER_R;
        case C_MENU: return PAD_BUTTON_START;
        default: return 0;
    }
}

void act(Ctl c)
{
    if (c >= C_HINT0 && c < C_HINT0 + s_nHints) {
        InputWiiU::inject(InputWiiU::buttonOf(s_hints[c - C_HINT0].button));
        return;
    }
    if (uint16_t b = padBits(c)) {
        InputWiiU::inject(b);
        /* the arrows lead: the pointer goes away */
        if (c != C_A) s_ptrUsed = 0;
        return;
    }
    switch (c) {
        case C_HERE: PlatformWiiU::setScreens(PlatformWiiU::Screens::GamePad); break;
        /* as the server's remote control does (PlaySession: a seek in a
         * transcode asks the server again) */
        case C_PLAY: Remote::local(Remote::Cmd::PlayPause); break;
        case C_BACK10: Remote::local(Remote::Cmd::Rewind); break;
        case C_FWD30: Remote::local(Remote::Cmd::FastForward); break;
        default: break;
    }
}

/* the touch, once a scan */
void handleTouch()
{
    if (SwkbdWiiU::shown()) { s_held = C_NONE; return; }
    const uint32_t scan = InputWiiU::scans();
    if (scan == s_lastScan) return;
    s_lastScan = scan;
    float x, y;
    bool tap, dragged;
    const bool down = InputWiiU::touch(x, y, tap, dragged);
    const uint64_t now = nowMs();
    if (down) {
        if (s_held == C_NONE) {
            s_held = ctlAt(x, y);
            s_heldSince = s_lastRepeat = now;
            s_lastTX = x; s_lastTY = y;
            if (s_held == C_PAD && !s_ptrUsed) { s_ptrX = 320; s_ptrY = 240; }
        }
        if (s_held == C_PAD) {
            /* the pad moves the TV's pointer (a little faster than the finger) */
            s_ptrX += (x - s_lastTX) * 1.4f;
            s_ptrY += (y - s_lastTY) * 1.4f;
            s_ptrX = s_ptrX < 0 ? 0 : s_ptrX > 640 ? 640 : s_ptrX;
            s_ptrY = s_ptrY < 0 ? 0 : s_ptrY > 480 ? 480 : s_ptrY;
            s_ptrUsed = now;
        } else if (s_held == C_BAR) {
            float f = (x - R_BAR.x) / R_BAR.w;
            s_barPick = f < 0 ? 0 : f > 1 ? 1 : f;
        } else if (s_held >= C_UP && s_held <= C_RIGHT && now - s_heldSince > 400 && now - s_lastRepeat > 110) {
            /* an arrow held: it repeats */
            s_lastRepeat = now;
            act(s_held);
        }
        s_lastTX = x; s_lastTY = y;
    } else if (s_held != C_NONE) {
        const Ctl c = s_held;
        s_held = C_NONE;
        if (c == C_PAD) {
            if (tap) { InputWiiU::inject(PAD_BUTTON_A); s_ptrUsed = now; }
        } else if (c == C_BAR) {
            if (s_barPick >= 0 && s_playDur > 0)
                Remote::local(Remote::Cmd::Seek, (long long)(s_barPick * s_playDur * 10000000.0));
            s_barPick = -1;
        } else if (ctlAt(s_lastTX, s_lastTY) == c && !(c >= C_UP && c <= C_RIGHT && now - s_heldSince > 400)) {
            act(c);
        }
    }
    /* the pointer shows for a while after the pad was used */
    const bool ptr = s_ptrUsed && now - s_ptrUsed < 8000 && !playing();
    InputWiiU::setRemotePointer(ptr, s_ptrX, s_ptrY);
}

/* ---- drawing ---------------------------------------------------------------- */

void key(const Rect& r, Ctl c, const char* label, int size, bool accent = false)
{
    const Ui::Palette& p = Ui::pal();
    const bool on = s_held == c;
    Ui::shadow(r.x, r.y + 2, r.w, r.h, 14, 6, p.shadow);
    u32 top = on ? p.accent : accent ? Ui::mix(p.cardTop, p.accent, 0.25f) : p.cardTop;
    u32 bottom = on ? p.accentDark : p.cardBottom;
    Ui::roundRect(r.x, r.y, r.w, r.h, 14, top, bottom);
    Ui::roundBorder(r.x, r.y, r.w, r.h, 14, 1.5f, on ? p.accentDark : p.cardBorder);
    if (label && *label)
        Ui::textCentered(r.x + r.w / 2, r.y + (r.h - size) / 2 - 2, label, size, on ? p.textOnAccent : p.text);
}

void arrow(const Rect& r, Ctl c, int dir)
{
    key(r, c, "", 0);
    const Ui::Palette& p = Ui::pal();
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, a = 14;
    const u32 col = s_held == c ? p.textOnAccent : p.text;
    switch (dir) {
        case 0: Ui::triangle(cx, cy - a, cx + a, cy + a * 0.6f, cx - a, cy + a * 0.6f, col); break;
        case 1: Ui::triangle(cx, cy + a, cx - a, cy - a * 0.6f, cx + a, cy - a * 0.6f, col); break;
        case 2: Ui::triangle(cx - a, cy, cx + a * 0.6f, cy - a, cx + a * 0.6f, cy + a, col); break;
        default: Ui::triangle(cx + a, cy, cx - a * 0.6f, cy + a, cx - a * 0.6f, cy - a, col); break;
    }
}

std::string fmtTime(float s)
{
    if (s < 0) s = 0;
    const int t = (int)s, h = t / 3600, m = t / 60 % 60, sec = t % 60;
    char b[16];
    if (h) snprintf(b, sizeof(b), "%d:%02d:%02d", h, m, sec);
    else snprintf(b, sizeof(b), "%d:%02d", m, sec);
    return b;
}

void drawPlaying()
{
    const Ui::Palette& p = Ui::pal();
    /* the picture itself behind the controls (the remote's space is
     * 854 units on GX's 640 frame pixels: the scissor's) */
    {
        const float k = 640.0f / 854.0f;
        const int cx0 = (int)ceilf(R_PAD.x * k), cx1 = (int)floorf((R_PAD.x + R_PAD.w) * k);
        GRRLIB_ClipDrawing(cx0, (int)R_PAD.y, cx1 - cx0, (int)R_PAD.h);
        const bool pic = PlayerWiiU::drawPreview(R_PAD.x, R_PAD.y, R_PAD.w, R_PAD.h);
        GRRLIB_ClipReset();
        if (pic) {
            /* darker where the text and the buttons are */
            GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
            Ui::roundRect(R_PAD.x, R_PAD.y, R_PAD.w, 90, 0, 0x000000B0, 0x00000000);
            Ui::roundRect(R_PAD.x, R_PAD.y + 120, R_PAD.w, R_PAD.h - 120, 0, 0x00000000, 0x000000C8);
        }
        Ui::roundBorder(R_PAD.x - 1, R_PAD.y - 1, R_PAD.w + 2, R_PAD.h + 2, 18, 3.0f, p.bgBottom);
        if (!pic) Ui::roundRect(R_PAD.x, R_PAD.y, R_PAD.w, R_PAD.h, 18, Ui::alpha(p.cardTop, 0.55f), Ui::alpha(p.cardBottom, 0.55f));
    }
    const char* title = s_playTitle.empty() ? tr("Now playing") : s_playTitle.c_str();
    Ui::text(R_PAD.x + 20, R_PAD.y + 18, title, 22, 0xFFFFFFFF);
    if (!s_playSub.empty()) Ui::text(R_PAD.x + 20, R_PAD.y + 52, s_playSub.c_str(), 15, 0xFFFFFFC0);
    /* the progress */
    const float dur = s_playDur, pos = s_playPos;
    float f = dur > 0 ? pos / dur : 0;
    if (s_barPick >= 0) f = s_barPick;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    const float by = R_BAR.y + R_BAR.h / 2 - 4;
    Ui::roundRect(R_BAR.x, by, R_BAR.w, 8, 4, p.field);
    Ui::roundRect(R_BAR.x, by, R_BAR.w * f, 8, 4, p.accent);
    Ui::circle(R_BAR.x + R_BAR.w * f, by + 4, s_held == C_BAR ? 12 : 9, p.accent);
    Ui::text(R_BAR.x, by + 16, fmtTime(s_barPick >= 0 ? s_barPick * dur : pos).c_str(), 14, 0xFFFFFFD0);
    if (dur > 0) Ui::textRight(R_BAR.x + R_BAR.w, by + 16, fmtTime(dur).c_str(), 14, 0xFFFFFFD0);
    /* the transport */
    key(R_BACK10, C_BACK10, "-10 s", 22);
    key(R_PLAY, C_PLAY, "", 0, true);
    {
        const float cx = R_PLAY.x + R_PLAY.w / 2, cy = R_PLAY.y + R_PLAY.h / 2;
        const u32 col = s_held == C_PLAY ? p.textOnAccent : p.text;
        if (g_mplayer_paused) Ui::triangle(cx - 12, cy - 18, cx + 18, cy, cx - 12, cy + 18, col);
        else { Ui::roundRect(cx - 14, cy - 17, 10, 34, 2, col); Ui::roundRect(cx + 4, cy - 17, 10, 34, 2, col); }
    }
    key(R_FWD30, C_FWD30, "+30 s", 22);
}

void drawPad()
{
    const Ui::Palette& p = Ui::pal();
    const bool on = s_held == C_PAD;
    Ui::roundRect(R_PAD.x, R_PAD.y, R_PAD.w, R_PAD.h, 18,
                  Ui::alpha(on ? Ui::mix(p.cardTop, p.accent, 0.15f) : p.cardTop, 0.55f), Ui::alpha(p.cardBottom, 0.55f));
    Ui::roundBorder(R_PAD.x, R_PAD.y, R_PAD.w, R_PAD.h, 18, 1.5f, on ? p.accent : p.cardBorder);
    const float cx = R_PAD.x + R_PAD.w / 2;
    Ui::textCentered(cx, R_PAD.y + R_PAD.h / 2 - 44, tr("Touch pad"), 22, p.text);
    Ui::textCentered(cx, R_PAD.y + R_PAD.h / 2 - 8, tr("Slide to move the pointer on the TV"), 15, p.textDim);
    Ui::textCentered(cx, R_PAD.y + R_PAD.h / 2 + 16, tr("Tap to choose"), 15, p.textDim);
    const char* np = Ui::nowPlaying();
    if (np && *np) Ui::textCentered(cx, R_PAD.y + R_PAD.h - 34, np, 14, p.textDim);
}

/* a hint as a button: its button's name (the Wii U's) and its label */
void hintKey(int i)
{
    const Rect r = hintRect(i);
    const Ctl c = (Ctl)(C_HINT0 + i);
    key(r, c, "", 0);
    const Ui::Palette& p = Ui::pal();
    const bool on = s_held == c;
    const char* b = Ui::buttonName(s_hints[i].button);
    const std::string label = tr(s_hints[i].label);
    const int bs = 16, ls = 15;
    const float bw = Ui::textWidth(b, bs) + 16, lw = Ui::textWidth(label.c_str(), ls);
    float x = r.x + (r.w - bw - 8 - lw) / 2;
    if (x < r.x + 6) x = r.x + 6;
    const float cy = r.y + r.h / 2;
    Ui::roundRect(x, cy - 13, bw, 26, 13, on ? p.accentDark : p.field);
    Ui::textCentered(x + bw / 2, cy - 10, b, bs, on ? p.textOnAccent : p.text);
    Ui::text(x + bw + 8, cy - 9, label.c_str(), ls, on ? p.textOnAccent : p.textDim);
}

/* the GamePad while WiiFin is on the TV */
void drawRemote()
{
    gatherHints();
    handleTouch();
    const Ui::Palette& p = Ui::pal();
    /* the background, the whole 854x480 */
    GRRLIB_Rectangle(0, 0, 854, 480, p.bgBottom, true);
    Ui::roundRect(-20, -20, 894, 520, 0, p.bgTop, p.bgBottom);
    /* the header: the app, what it is, the clock */
    Ui::text(24, 18, "WiiFin", 24, p.accent);
    Ui::text(24 + Ui::textWidth("WiiFin", 24) + 12, 25, tr("Remote control"), 16, p.textDim);
    {
        time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        char clock[16];
        if (Ui::clock12h())
            snprintf(clock, sizeof(clock), "%d:%02d %s", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min,
                     lt.tm_hour < 12 ? "AM" : "PM");
        else
            snprintf(clock, sizeof(clock), "%02d:%02d", lt.tm_hour, lt.tm_min);
        Ui::textRight(830, 20, clock, 22, p.text);
    }
    if (playing()) drawPlaying();
    else drawPad();
    arrow(R_UP, C_UP, 0);
    arrow(R_DOWN, C_DOWN, 1);
    arrow(R_LEFT, C_LEFT, 2);
    arrow(R_RIGHT, C_RIGHT, 3);
    key(R_A, C_A, "A", 26, true);
    key(R_L, C_L, "L", 20);
    key(R_B, C_B, "B", 22);
    key(R_R, C_R, "R", 20);
    key(R_MENU, C_MENU, "+", 24);
    key(R_HERE, C_HERE, tr("Show on the GamePad"), 15);
    for (int i = 0; i < s_nHints; ++i) hintKey(i);
}

/* ---- the screens --------------------------------------------------------------- */

bool s_tvPresent = true;
uint64_t s_tvChecked = 0;

bool detectTv()
{
    TVEPort port = TVE_PORT_HDMI;
    if (AVMGetCurrentPort(&port) && port != TVE_PORT_HDMI) return true;   /* analog: can't tell, assumed */
    TVEHdmiState st = TVE_HDMI_STATE_NONE;
    AVMGetHDMIState(&st);
    return st != TVE_HDMI_STATE_NONE && st != TVE_HDMI_STATE_HTPG_OFF && st != TVE_HDMI_STATE_SHUTDOWN;
}

void applyScreens()
{
    bool tv;
    switch (s_screens) {
        case PlatformWiiU::Screens::TV: tv = true; break;
        case PlatformWiiU::Screens::GamePad: tv = false; break;
        default: tv = s_tvPresent; break;
    }
    GfxWiiU::setMode(tv ? GfxWiiU::Mode::Remote : GfxWiiU::Mode::Mirror);
}

void keepAwake(bool video, bool playing);

/* once a frame (the render hook): L3 + R3, a TV plugged or unplugged */
void (*s_prevBeforeRender)() = nullptr;
void beforeRender()
{
    Ui::endFrameHints();
    if (InputWiiU::swapScreensAsked()) {
        static uint32_t s_swapScan = 0;
        if (InputWiiU::scans() != s_swapScan) {
            s_swapScan = InputWiiU::scans();
            PlatformWiiU::setScreens(GfxWiiU::mode() == GfxWiiU::Mode::Remote ? PlatformWiiU::Screens::GamePad
                                                                              : PlatformWiiU::Screens::TV);
        }
    }
    const uint64_t now = nowMs();
    if (now - s_tvChecked > 2000) {
        s_tvChecked = now;
        const bool tv = detectTv();
        if (tv != s_tvPresent) {
            s_tvPresent = tv;
            SYS_Report("[PlatformWiiU] TV %s\n", tv ? "plugged in" : "unplugged");
            if (s_screens == PlatformWiiU::Screens::Auto) applyScreens();
        }
    }
    {
        const bool video = PlayerWiiU::videoActive() && !g_mplayer_paused;
        const char* np = Ui::nowPlaying();
        keepAwake(video, video || (np && *np));
    }
    if (s_prevBeforeRender) s_prevBeforeRender();
}

/* The screens stay lit while a video plays, and the console on while
 * something plays (its auto power-down otherwise: an hour without a
 * button); the system's choices back after */
uint32_t s_dimWas = 1, s_apdWas = 1;
bool s_imRead = false, s_dimOff = false, s_apdOff = false;
void keepAwake(bool video, bool playing)
{
    if (!s_imRead) {
        IMIsDimEnabled(&s_dimWas);
        IMIsAPDEnabled(&s_apdWas);
        s_imRead = true;
    }
    if (video != s_dimOff) {
        if (video) IMDisableDim();
        else if (s_dimWas) IMEnableDim();
        s_dimOff = video;
    }
    if (playing != s_apdOff) {
        if (playing) IMDisableAPD();
        else if (s_apdWas) IMEnableAPD();
        s_apdOff = playing;
    }
}
void restoreAwake() { keepAwake(false, false); }

/* The HOME menu: a video pauses (as the Wii's HOME menu does) */
bool s_pausedForHome = false;
void onForeground(bool leaving)
{
    if (leaving) {
        s_pausedForHome = PlayerWiiU::videoActive() && !g_mplayer_paused;
        if (s_pausedForHome) wii_player_pause_toggle();
        SYS_Report("[PlatformWiiU] to the background%s\n", s_pausedForHome ? " (video paused)" : "");
    } else {
        SYS_Report("[PlatformWiiU] back\n");
    }
}

} // namespace

extern "C" void (*g_wiifin_before_render)(void);

namespace PlatformWiiU {

void init()
{
    GfxWiiU::init();
    s_tvPresent = detectTv();
    {
        TVEPort port = TVE_PORT_HDMI;
        AVMGetCurrentPort(&port);
        TVEHdmiState st = TVE_HDMI_STATE_NONE;
        AVMGetHDMIState(&st);
        int w, h;
        GfxWiiU::tvSize(w, h);
        SYS_Report("[PlatformWiiU] TV port %d, HDMI state %d, %dx%d: %s\n", (int)port, (int)st, w, h,
                   s_tvPresent ? "a TV" : "no TV");
    }
    applyScreens();
    GfxWiiU::setRemoteDrawer(drawRemote);
    GfxWiiU::setForegroundHook(onForeground);
    s_prevBeforeRender = g_wiifin_before_render;
    g_wiifin_before_render = beforeRender;
    /* a video app: the system's longer auto power-down; ours back at exit */
    IMStartAPDVideoMode();
    atexit(restoreAwake);
    AudioWiiU::init();
}

void setScreens(Screens s)
{
    s_screens = s;
    applyScreens();
}

Screens screens() { return s_screens; }
bool tvPresent() { return s_tvPresent; }

void setPlayTimes(float pos, float dur)
{
    s_playPos = pos;
    s_playDur = dur;
}

void setNowPlaying(const char* title, const char* subtitle)
{
    s_playTitle = title ? title : "";
    s_playSub = subtitle ? subtitle : "";
}

}
