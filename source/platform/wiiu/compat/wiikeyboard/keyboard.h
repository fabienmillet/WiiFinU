#pragma once
/* no USB keyboard on the Wii U (yet) */
#include "../gctypes.h"
typedef enum { KEYBOARD_PRESSED = 0, KEYBOARD_RELEASED, KEYBOARD_DISCONNECTED, KEYBOARD_CONNECTED } keyboard_event_type;
typedef struct { keyboard_event_type type; u16 modifiers; u8 keycode; u16 symbol; } keyboard_event;
static inline s32 KEYBOARD_Init(void* cb) { (void)cb; return -1; }
static inline s32 KEYBOARD_Deinit(void) { return 0; }
static inline s32 KEYBOARD_GetEvent(keyboard_event* e) { (void)e; return 0; }
