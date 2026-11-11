/* -----------------------------------------------------------------------
 * InputWiiU — the GamePad (VPAD), Wii Remotes, Pro and Classic
 * Controllers (KPAD) as the Wii's controls, see InputWiiU.h.
 * ----------------------------------------------------------------------- */
#include "InputWiiU.h"
#include "GfxWiiU.h"
#include "SwkbdWiiU.h"

#include <vpad/input.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <coreinit/time.h>
#include <string.h>
#include <math.h>

/* libogc's bit values (compat/wiiuse/wpad.h; not included here: wut's
 * padscore names the same things) */
namespace ogc {
const uint32_t B2 = 0x0001, B1 = 0x0002, B = 0x0004, A = 0x0008, MINUS = 0x0010, HOME = 0x0080,
               LEFT = 0x0100, RIGHT = 0x0200, DOWN = 0x0400, UP = 0x0800, PLUS = 0x1000;
/* the GameCube controller's (ogc/pad.h) */
const uint16_t P_LEFT = 0x0001, P_RIGHT = 0x0002, P_DOWN = 0x0004, P_UP = 0x0008, P_Z = 0x0010,
               P_R = 0x0020, P_L = 0x0040, P_A = 0x0100, P_B = 0x0200, P_X = 0x0400, P_Y = 0x0800,
               P_START = 0x1000;
}

/* Ui.cpp: the hint drawn at (x, y) lately (a tap there is its button) */
namespace Ui { const char* hintAt(float x, float y); }

namespace {

InputWiiU::State s_st;
uint32_t s_prevCore = 0;
uint16_t s_inject = 0;
uint32_t s_scans = 0;
bool     s_remotePtr = false;
float    s_remoteX = 0, s_remoteY = 0;
uint64_t s_lastInputMs = 0;
bool     s_swapAsked = false;

/* touch, in the GamePad's pixels */
bool     s_touching = false, s_tap = false, s_dragged = false;
float    s_tx = 0, s_ty = 0, s_startX = 0, s_startY = 0;
uint64_t s_touchStart = 0;
bool     s_kpadUp = false;
VPADStatus s_rawVpad;
bool       s_rawVpadOk = false;
KPADStatus s_rawKpad[4];
bool       s_rawKpadOk[4] = {};

uint64_t nowMs() { return (uint64_t)OSTicksToMilliseconds(OSGetSystemTime()); }

uint16_t fromVpad(uint32_t v)
{
    static const struct { uint32_t from; uint16_t to; } MAP[] = {
        { VPAD_BUTTON_A, ogc::P_A }, { VPAD_BUTTON_B, ogc::P_B }, { VPAD_BUTTON_X, ogc::P_X },
        { VPAD_BUTTON_Y, ogc::P_Y }, { VPAD_BUTTON_UP, ogc::P_UP }, { VPAD_BUTTON_DOWN, ogc::P_DOWN },
        { VPAD_BUTTON_LEFT, ogc::P_LEFT }, { VPAD_BUTTON_RIGHT, ogc::P_RIGHT }, { VPAD_BUTTON_L, ogc::P_L },
        { VPAD_BUTTON_R, ogc::P_R }, { VPAD_BUTTON_ZL, ogc::P_Z }, { VPAD_BUTTON_ZR, ogc::P_Z },
        { VPAD_BUTTON_MINUS, ogc::P_Z }, { VPAD_BUTTON_PLUS, ogc::P_START },
    };
    uint16_t out = 0;
    for (const auto& m : MAP) if (v & m.from) out |= m.to;
    return out;
}

/* a Pro or Classic Controller (the same bits in wut) */
uint16_t fromClassic(uint32_t v)
{
    static const struct { uint32_t from; uint16_t to; } MAP[] = {
        { WPAD_CLASSIC_BUTTON_A, ogc::P_A }, { WPAD_CLASSIC_BUTTON_B, ogc::P_B },
        { WPAD_CLASSIC_BUTTON_X, ogc::P_X }, { WPAD_CLASSIC_BUTTON_Y, ogc::P_Y },
        { WPAD_CLASSIC_BUTTON_UP, ogc::P_UP }, { WPAD_CLASSIC_BUTTON_DOWN, ogc::P_DOWN },
        { WPAD_CLASSIC_BUTTON_LEFT, ogc::P_LEFT }, { WPAD_CLASSIC_BUTTON_RIGHT, ogc::P_RIGHT },
        { WPAD_CLASSIC_BUTTON_L, ogc::P_L }, { WPAD_CLASSIC_BUTTON_R, ogc::P_R },
        { WPAD_CLASSIC_BUTTON_ZL, ogc::P_Z }, { WPAD_CLASSIC_BUTTON_ZR, ogc::P_Z },
        { WPAD_CLASSIC_BUTTON_MINUS, ogc::P_Z }, { WPAD_CLASSIC_BUTTON_PLUS, ogc::P_START },
    };
    uint16_t out = 0;
    for (const auto& m : MAP) if (v & m.from) out |= m.to;
    return out;
}

uint32_t fromRemote(uint32_t w)
{
    static const struct { uint32_t from, to; } MAP[] = {
        { WPAD_BUTTON_A, ogc::A }, { WPAD_BUTTON_B, ogc::B }, { WPAD_BUTTON_1, ogc::B1 },
        { WPAD_BUTTON_2, ogc::B2 }, { WPAD_BUTTON_MINUS, ogc::MINUS }, { WPAD_BUTTON_PLUS, ogc::PLUS },
        { WPAD_BUTTON_HOME, ogc::HOME }, { WPAD_BUTTON_UP, ogc::UP }, { WPAD_BUTTON_DOWN, ogc::DOWN },
        { WPAD_BUTTON_LEFT, ogc::LEFT }, { WPAD_BUTTON_RIGHT, ogc::RIGHT },
    };
    uint32_t out = 0;
    for (const auto& m : MAP) if (w & m.from) out |= m.to;
    return out;
}

/* a hint's button (Wii names) as the GameCube bits Input.cpp reads */
uint16_t hintBits(const char* b)
{
    static const struct { const char* wii; uint16_t bits; } MAP[] = {
        { "A", ogc::P_A }, { "B", ogc::P_B }, { "1", ogc::P_Y }, { "2", ogc::P_X },
        { "-", ogc::P_L }, { "+", ogc::P_R }, { "HOME", ogc::P_START }, { "+!", ogc::P_Z }, { "ZR", ogc::P_Z },
    };
    for (const auto& m : MAP)
        if (strcmp(b, m.wii) == 0) return m.bits;
    return 0;
}

} // namespace

namespace InputWiiU {

void scan()
{
    const uint64_t now = nowMs();
    if (!s_kpadUp) {
        KPADInit();
        WPADEnableURCC(TRUE);
        for (int c = 0; c < 4; ++c) KPADEnableDPD((KPADChan)c);
        s_kpadUp = true;
    }
    ++s_scans;
    uint32_t core = 0;
    uint16_t pad = 0;
    float lx = 0, ly = 0;
    bool ptrValid = false;
    float px = 0, py = 0;

    /* the GamePad */
    VPADStatus vs;
    VPADReadError ve = VPAD_READ_SUCCESS;
    memset(&vs, 0, sizeof(vs));
    static VPADStatus s_lastVs;
    static bool s_haveVs = false;
    s_rawVpadOk = false;
    if (VPADRead(VPAD_CHAN_0, &vs, 1, &ve) > 0 && ve == VPAD_READ_SUCCESS) {
        s_lastVs = vs; s_haveVs = true;
        s_rawVpad = vs; s_rawVpadOk = true;
    }
    else if (s_haveVs && ve == VPAD_READ_NO_SAMPLES) {
        /* nothing new: held as it was, nothing pressed or let go again */
        vs = s_lastVs;
        vs.trigger = vs.release = 0;
    }
    else s_haveVs = false;
    if (s_haveVs) {
        pad |= fromVpad(vs.hold);
        lx = vs.leftStick.x; ly = vs.leftStick.y;
        s_swapAsked = (vs.trigger & (VPAD_BUTTON_STICK_L | VPAD_BUTTON_STICK_R)) &&
                      (vs.hold & VPAD_BUTTON_STICK_L) && (vs.hold & VPAD_BUTTON_STICK_R);
        /* the touch screen */
        VPADTouchData tp;
        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &tp, &vs.tpNormal);
        s_tap = false;
        if (tp.touched && tp.validity == VPAD_VALID) {
            const float x = tp.x * (854.0f / 1280.0f), y = tp.y * (480.0f / 720.0f);
            if (!s_touching) {
                s_touching = true;
                s_dragged = false;
                s_startX = x; s_startY = y;
                s_touchStart = now;
            } else if ((x - s_startX) * (x - s_startX) + (y - s_startY) * (y - s_startY) > 20.0f * 20.0f) {
                s_dragged = true;
            }
            s_tx = x; s_ty = y;
            s_lastInputMs = now;
        } else if (s_touching && !tp.touched) {
            s_touching = false;
            s_tap = !s_dragged && now - s_touchStart < 800;
        }
        if (vs.hold || fabsf(lx) > 0.3f || fabsf(ly) > 0.3f) s_lastInputMs = now;
    } else {
        s_touching = false;
        s_tap = false;
    }

    /* Wii Remotes, Pro and Classic Controllers */
    for (int c = 0; c < 4; ++c) {
        KPADStatus ks;
        KPADError ke = KPAD_ERROR_OK;
        memset(&ks, 0, sizeof(ks));
        s_rawKpadOk[c] = false;
        static KPADStatus s_lastKs[4];
        static bool s_haveKs[4] = {};
        if (KPADReadEx((KPADChan)c, &ks, 1, &ke) > 0 && ke == KPAD_ERROR_OK) {
            s_lastKs[c] = ks;
            s_haveKs[c] = true;
            s_rawKpad[c] = ks;
            s_rawKpadOk[c] = true;
        } else if (ke == KPAD_ERROR_NO_SAMPLES && s_haveKs[c]) {
            /* nothing new: still held (else a button held came back as a press) */
            ks = s_lastKs[c];
            ks.trigger = ks.release = 0;
            ks.classic.trigger = ks.classic.release = 0;
        } else {
            s_haveKs[c] = false;
            continue;
        }
        if (ks.extensionType == WPAD_EXT_PRO_CONTROLLER) {
            pad |= fromClassic(ks.pro.hold);
            if (fabsf(ks.pro.leftStick.x) > fabsf(lx)) lx = ks.pro.leftStick.x;
            if (fabsf(ks.pro.leftStick.y) > fabsf(ly)) ly = ks.pro.leftStick.y;
            if (ks.pro.hold) s_lastInputMs = now;
            continue;
        }
        core |= fromRemote(ks.hold);
        if (ks.extensionType == WPAD_EXT_CLASSIC || ks.extensionType == WPAD_EXT_MPLUS_CLASSIC) {
            pad |= fromClassic(ks.classic.hold);
            if (fabsf(ks.classic.leftStick.x) > fabsf(lx)) lx = ks.classic.leftStick.x;
            if (fabsf(ks.classic.leftStick.y) > fabsf(ly)) ly = ks.classic.leftStick.y;
        }
        /* the first Wii Remote pointing at the screen is the pointer */
        if (!ptrValid && ks.posValid > 0 && ks.pos.x > -1.2f && ks.pos.x < 1.2f && ks.pos.y > -1.2f && ks.pos.y < 1.2f) {
            ptrValid = true;
            px = (ks.pos.x + 1.0f) * 320.0f;
            py = (ks.pos.y + 1.0f) * 240.0f;
        }
        if (ks.hold || ks.classic.hold) s_lastInputMs = now;
    }

    /* the touch screen as the pointer when it shows WiiFin, else the
     * remote control's pad */
    uint16_t inject = s_inject;
    s_inject = 0;
    const bool mirror = GfxWiiU::mode() == GfxWiiU::Mode::Mirror;
    bool fromTouch = false;
    if (mirror && (s_touching || s_tap)) {
        ptrValid = true;
        fromTouch = true;
        px = s_tx * (640.0f / 854.0f);
        py = s_ty * (480.0f / 480.0f);
        if (s_tap) {
            /* on a button hint: that button rather than A where it is */
            const char* h = Ui::hintAt(s_tx - 107.0f, s_ty);
            const uint16_t hb = h ? hintBits(h) : 0;
            inject |= hb ? hb : ogc::P_A;
        }
    } else if (!mirror && s_remotePtr && !ptrValid) {
        ptrValid = true;
        px = s_remoteX; py = s_remoteY;
    }
    if (inject) s_lastInputMs = now;
    GfxWiiU::hideCursor(fromTouch);

    if (SwkbdWiiU::shown()) {
        /* the system's keyboard has the controls */
        core = 0; pad = 0; inject = 0; lx = ly = 0;
        ptrValid = false;
        s_tap = false;
    }
    /* an injected press is held for this scan only (Input.cpp sees it
     * pressed, then released) */
    s_st.core = core;
    s_st.coreDown = core & ~s_prevCore;
    s_prevCore = core;
    s_st.pad = pad | inject;
    s_st.lx = lx; s_st.ly = ly;
    s_st.ptrValid = ptrValid;
    s_st.ptrX = px; s_st.ptrY = py;
}

uint32_t scans() { return s_scans; }
const void* rawVpad() { return s_rawVpadOk ? &s_rawVpad : nullptr; }
const void* rawKpad(int c) { return c >= 0 && c < 4 && s_rawKpadOk[c] ? &s_rawKpad[c] : nullptr; }

const State& state() { return s_st; }

bool touch(float& x, float& y, bool& tap, bool& dragged)
{
    x = s_tx; y = s_ty;
    tap = s_tap;
    dragged = s_dragged;
    return s_touching;
}

void inject(uint16_t bits) { s_inject |= bits; }
uint16_t buttonOf(const char* wiiName) { return wiiName ? hintBits(wiiName) : 0; }

void setRemotePointer(bool valid, float fx, float fy)
{
    s_remotePtr = valid;
    s_remoteX = fx; s_remoteY = fy;
}

uint64_t lastInputMs() { return s_lastInputMs; }
bool swapScreensAsked() { return s_swapAsked; }

}
