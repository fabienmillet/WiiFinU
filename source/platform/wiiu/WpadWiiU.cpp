/* -----------------------------------------------------------------------
 * libogc's WPAD and PAD on the Wii U: InputWiiU's state (see InputWiiU.h)
 * as the Wii's Wii Remote 1 (a Wii Remote, the pointer) and GameCube
 * controller 1 (the GamePad, a Pro or Classic Controller).
 * ----------------------------------------------------------------------- */
#include "InputWiiU.h"
#include <string.h>
#include <math.h>
#include <wiiuse/wpad.h>
#include <ogc/pad.h>

namespace {
WPADData s_data;
}

extern "C" {

s32 WPAD_Init(void) { return 0; }

s32 WPAD_ScanPads(void)
{
    InputWiiU::scan();
    const InputWiiU::State& s = InputWiiU::state();
    memset(&s_data, 0, sizeof(s_data));
    s_data.exp.type = WPAD_EXP_NONE;
    s_data.btns_h = s.core;
    s_data.btns_d = s.coreDown;
    return 0;
}

u32 WPAD_ButtonsDown(int chan) { return chan == 0 ? s_data.btns_d : 0; }
u32 WPAD_ButtonsHeld(int chan) { return chan == 0 ? s_data.btns_h : 0; }
u32 WPAD_ButtonsUp(int chan) { (void)chan; return 0; }

void WPAD_IR(int chan, ir_t* ir)
{
    memset(ir, 0, sizeof(*ir));
    if (chan != 0) return;
    const InputWiiU::State& s = InputWiiU::state();
    ir->valid = s.ptrValid;
    ir->x = s.ptrX;
    ir->y = s.ptrY;
    ir->smooth_valid = ir->raw_valid = ir->valid;
    ir->sx = ir->ax = s.ptrX;
    ir->sy = ir->ay = s.ptrY;
}

void WPAD_Orientation(int chan, orient_t* o) { (void)chan; memset(o, 0, sizeof(*o)); }

WPADData* WPAD_Data(int chan) { (void)chan; return &s_data; }

s32 WPAD_Probe(int chan, u32* type)
{
    if (type) *type = chan == 0 ? (u32)s_data.exp.type : WPAD_EXP_NONE;
    return chan == 0 ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER;
}

u32 PAD_Init(void) { return 1; }
/* always answered (Input.cpp: else a press could come twice) */
u32 PAD_ScanPads(void) { return 1; }
u16 PAD_ButtonsHeld(int pad) { return pad == 0 ? InputWiiU::state().pad : 0; }
u16 PAD_ButtonsDown(int pad) { (void)pad; return 0; }   /* Input.cpp computes its own */

/* the stick (-1..1) as the GameCube's (about +-100) */
static s8 stick(float v)
{
    int i = (int)(v * 100.0f);
    return (s8)(i > 127 ? 127 : i < -128 ? -128 : i);
}
s8 PAD_StickX(int pad) { return pad == 0 ? stick(InputWiiU::state().lx) : 0; }
s8 PAD_StickY(int pad) { return pad == 0 ? stick(InputWiiU::state().ly) : 0; }

} // extern "C"
