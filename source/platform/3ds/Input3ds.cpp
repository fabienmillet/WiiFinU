/* -----------------------------------------------------------------------
 * The 3DS's controls as the Wii's (see compat/wiiuse/wpad.h):
 *   - the buttons, circle pad and ZL / ZR as a GameCube controller (A, B,
 *     X, Y, L, R, START, Z: Input.cpp maps them as on the Wii; SELECT is
 *     Z too, the video's zoom, for the 3DS without ZL / ZR);
 *   - the touch screen as the Wii Remote's pointer: where the stylus is,
 *     and a tap (touch and lift without dragging) is A where it lifted.
 * Touch coordinates are the bottom screen's (320x240): x2 for the 640x480
 * frame drawn on it.
 * ----------------------------------------------------------------------- */
#include <3ds.h>
#include <string.h>
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <ogc/lwp_watchdog.h>

namespace {

u32  s_held = 0, s_down = 0;
bool s_touching = false;
bool s_tap = false;                 /* a tap ended this frame */
float s_x = 0, s_y = 0;             /* last touch, frame pixels */
float s_startX = 0, s_startY = 0;
u64  s_touchStart = 0;
bool s_dragged = false;
circlePosition s_cpad = { 0, 0 };
WPADData s_data;

u16 toPad(u32 k)
{
    u16 p = 0;
    if (k & KEY_A)      p |= PAD_BUTTON_A;
    if (k & KEY_B)      p |= PAD_BUTTON_B;
    if (k & KEY_X)      p |= PAD_BUTTON_X;
    if (k & KEY_Y)      p |= PAD_BUTTON_Y;
    if (k & KEY_L)      p |= PAD_TRIGGER_L;
    if (k & KEY_R)      p |= PAD_TRIGGER_R;
    if (k & (KEY_ZL | KEY_ZR | KEY_SELECT)) p |= PAD_TRIGGER_Z;
    if (k & KEY_START)  p |= PAD_BUTTON_START;
    if (k & KEY_DUP)    p |= PAD_BUTTON_UP;
    if (k & KEY_DDOWN)  p |= PAD_BUTTON_DOWN;
    if (k & KEY_DLEFT)  p |= PAD_BUTTON_LEFT;
    if (k & KEY_DRIGHT) p |= PAD_BUTTON_RIGHT;
    return p;
}

} // namespace

extern "C" {

s32 WPAD_Init(void) { return 0; }

/* when a button or the touch screen was last used (Platform3ds: the
 * bottom screen's light during a video) */
u64 g_3ds_lastInputMs = 0;

s32 WPAD_ScanPads(void)
{
    hidScanInput();
    s_held = hidKeysHeld();
    s_down = hidKeysDown();
    {
        circlePosition c;
        hidCircleRead(&c);
        if (s_held || c.dx * c.dx + c.dy * c.dy > 40 * 40) g_3ds_lastInputMs = ticks_to_millisecs(gettime());
    }
    hidCircleRead(&s_cpad);
    s_tap = false;
    const u64 now = ticks_to_millisecs(gettime());
    if (s_held & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        const float x = t.px * 2.0f, y = t.py * 2.0f;
        if (!s_touching) {
            s_touching = true;
            s_dragged = false;
            s_startX = x; s_startY = y;
            s_touchStart = now;
        } else if ((x - s_startX) * (x - s_startX) + (y - s_startY) * (y - s_startY) > 24.0f * 24.0f) {
            s_dragged = true;
        }
        s_x = x; s_y = y;
    } else if (s_touching) {
        s_touching = false;
        /* a tap: lifted where it touched, not held for long */
        s_tap = !s_dragged && now - s_touchStart < 800;
    }
    return 0;
}

u32 WPAD_ButtonsDown(int chan) { return chan == 0 && s_tap ? WPAD_BUTTON_A : 0; }
u32 WPAD_ButtonsHeld(int chan) { (void)chan; return 0; }
u32 WPAD_ButtonsUp(int chan) { (void)chan; return 0; }

void WPAD_IR(int chan, ir_t* ir)
{
    memset(ir, 0, sizeof(*ir));
    if (chan != 0) return;
    ir->valid = s_touching || s_tap;
    ir->x = s_x;
    ir->y = s_y;
    ir->smooth_valid = ir->raw_valid = ir->valid;
    ir->sx = ir->ax = s_x;
    ir->sy = ir->ay = s_y;
}

void WPAD_Orientation(int chan, orient_t* o) { (void)chan; memset(o, 0, sizeof(*o)); }

WPADData* WPAD_Data(int chan)
{
    (void)chan;
    s_data.exp.type = WPAD_EXP_NONE;
    return &s_data;
}

s32 WPAD_Probe(int chan, u32* type)
{
    if (type) *type = WPAD_EXP_NONE;
    return chan == 0 ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER;
}

u32 PAD_Init(void) { return 1; }
u32 PAD_ScanPads(void) { return 1; }
u16 PAD_ButtonsHeld(int pad) { return pad == 0 ? toPad(s_held) : 0; }
u16 PAD_ButtonsDown(int pad) { return pad == 0 ? toPad(s_down) : 0; }

/* the circle pad (about +-155) as the GameCube stick (about +-100) */
s8 PAD_StickX(int pad)
{
    if (pad != 0) return 0;
    int v = s_cpad.dx * 100 / 155;
    return (s8)(v > 127 ? 127 : v < -128 ? -128 : v);
}

s8 PAD_StickY(int pad)
{
    if (pad != 0) return 0;
    int v = s_cpad.dy * 100 / 155;
    return (s8)(v > 127 ? 127 : v < -128 ? -128 : v);
}

} // extern "C"
