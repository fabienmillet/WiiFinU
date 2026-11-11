#pragma once
/* -----------------------------------------------------------------------
 * InputWiiU — the Wii U's controls, read once a frame (WPAD_ScanPads):
 *   - the GamePad's buttons and sticks, a Pro Controller's or a Classic
 *     Controller's: a GameCube controller, as the 3DS's buttons (Y "1",
 *     X "2", L/R "-"/"+", + WiiFin's menu, ZL/ZR/- the zoom), named as
 *     the Wii U's (Ui.cpp); a Wii Remote: itself, its pointer the Wii's;
 *   - the GamePad's touch screen: when it shows WiiFin (Mirror mode), the
 *     pointer, a tap is A where it ends (or the button of a hint tapped);
 *     in Remote mode it is the remote control's (PlatformWiiU), which
 *     reads it here and sends buttons and a pointer of its own.
 * Plain types only: compat/wiiuse/wpad.h and wut's padscore clash.
 * ----------------------------------------------------------------------- */
#include <stdint.h>

namespace InputWiiU {

/* The state of a scan, in libogc's bits (WpadWiiU.cpp hands it on) */
struct State {
    uint32_t core;            /* Wii Remote buttons held (libogc WPAD_BUTTON_*) */
    uint32_t coreDown;
    uint16_t pad;             /* GameCube buttons held (libogc PAD_*) */
    float    lx, ly;          /* left stick, -1..1 (up +) */
    bool     ptrValid;        /* the pointer, frame pixels (640x480) */
    float    ptrX, ptrY;
};

void scan();
/* scans so far (the remote control acts once a scan) */
uint32_t scans();
const State& state();

/* The touch screen in the GamePad's 854x480 pixels: whether it is touched,
 * where, a tap ended this scan, a drag under way */
bool touch(float& x, float& y, bool& tap, bool& dragged);

/* From the remote control: GameCube buttons pressed once (libogc PAD_*),
 * and a pointer on the TV moved by the touch pad */
void inject(uint16_t padBits);
void setRemotePointer(bool valid, float frameX, float frameY);

/* Something was pressed or touched lately (ms, gettime's) */
uint64_t lastInputMs();
/* The GamePad's L3 + R3 pressed together this scan: the screens' mode */
bool swapScreensAsked();

}
