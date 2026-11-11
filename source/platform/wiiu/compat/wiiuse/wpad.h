#pragma once
/* -----------------------------------------------------------------------
 * The Wii Remote on the Wii U (InputWiiU.cpp): a Wii Remote (KPAD) is
 * itself, its pointer the pointer; the GamePad, a Pro Controller or a
 * Classic Controller come as a Classic Controller (their buttons have its
 * names); the GamePad's touch screen is the pointer when it shows WiiFin.
 * ----------------------------------------------------------------------- */
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
#define WPAD_CHAN_0 0
#define WPAD_CHAN_1 1
#define WPAD_CHAN_ALL -1
#define WPAD_BALANCE_BOARD 4
#define WPAD_MAX_WIIMOTES 4
#define WPAD_ERR_NONE 0
#define WPAD_ERR_NO_CONTROLLER -1
#define WPAD_FMT_BTNS 0
#define WPAD_FMT_BTNS_ACC 1
#define WPAD_FMT_BTNS_ACC_IR 2
#define WPAD_EXP_NONE 0
#define WPAD_EXP_NUNCHUK 1
#define WPAD_EXP_CLASSIC 2

#define WPAD_BUTTON_2     0x0001
#define WPAD_BUTTON_1     0x0002
#define WPAD_BUTTON_B     0x0004
#define WPAD_BUTTON_A     0x0008
#define WPAD_BUTTON_MINUS 0x0010
#define WPAD_BUTTON_HOME  0x0080
#define WPAD_BUTTON_LEFT  0x0100
#define WPAD_BUTTON_RIGHT 0x0200
#define WPAD_BUTTON_DOWN  0x0400
#define WPAD_BUTTON_UP    0x0800
#define WPAD_BUTTON_PLUS  0x1000
#define WPAD_NUNCHUK_BUTTON_Z (0x0001u << 16)
#define WPAD_NUNCHUK_BUTTON_C (0x0002u << 16)
#define WPAD_CLASSIC_BUTTON_UP     (0x0001u << 16)
#define WPAD_CLASSIC_BUTTON_LEFT   (0x0002u << 16)
#define WPAD_CLASSIC_BUTTON_ZR     (0x0004u << 16)
#define WPAD_CLASSIC_BUTTON_X      (0x0008u << 16)
#define WPAD_CLASSIC_BUTTON_A      (0x0010u << 16)
#define WPAD_CLASSIC_BUTTON_Y      (0x0020u << 16)
#define WPAD_CLASSIC_BUTTON_B      (0x0040u << 16)
#define WPAD_CLASSIC_BUTTON_ZL     (0x0080u << 16)
#define WPAD_CLASSIC_BUTTON_FULL_R (0x0200u << 16)
#define WPAD_CLASSIC_BUTTON_PLUS   (0x0400u << 16)
#define WPAD_CLASSIC_BUTTON_HOME   (0x0800u << 16)
#define WPAD_CLASSIC_BUTTON_MINUS  (0x1000u << 16)
#define WPAD_CLASSIC_BUTTON_FULL_L (0x2000u << 16)
#define WPAD_CLASSIC_BUTTON_DOWN   (0x4000u << 16)
#define WPAD_CLASSIC_BUTTON_RIGHT  (0x8000u << 16)

typedef struct { int valid; float x, y; float angle; float distance, z; int smooth_valid; float sx, sy; int raw_valid; float ax, ay; } ir_t;
typedef struct { float roll, pitch, yaw, a_roll, a_pitch; } orient_t;
typedef struct { float ang, mag; } joystick_t;
typedef struct { joystick_t ljs, rjs; float l_shoulder, r_shoulder; u32 btns, btns_held; } classic_ctrl_t;
typedef struct { int type; union { classic_ctrl_t classic; }; } expansion_t;
typedef struct { s32 err; u32 data_present; u32 btns_h, btns_l, btns_d, btns_u; ir_t ir; orient_t orient; expansion_t exp; } WPADData;

s32  WPAD_Init(void);
s32  WPAD_ScanPads(void);
u32  WPAD_ButtonsDown(int chan);
u32  WPAD_ButtonsHeld(int chan);
u32  WPAD_ButtonsUp(int chan);
void WPAD_IR(int chan, ir_t* ir);
void WPAD_Orientation(int chan, orient_t* o);
WPADData* WPAD_Data(int chan);
s32  WPAD_Probe(int chan, u32* type);
static inline s32 WPAD_SetDataFormat(int chan, int fmt) { (void)chan; (void)fmt; return 0; }
static inline s32 WPAD_SetVRes(int chan, u32 x, u32 y) { (void)chan; (void)x; (void)y; return 0; }
static inline s32 WPAD_Shutdown(void) { return 0; }
static inline s32 WPAD_Disconnect(int chan) { (void)chan; return 0; }
static inline void WPAD_Rumble(int chan, int on) { (void)chan; (void)on; }
#ifdef __cplusplus
}
#endif
