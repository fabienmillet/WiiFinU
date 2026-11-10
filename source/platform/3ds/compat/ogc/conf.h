#pragma once
/* The Wii's settings (SYSCONF) from the 3DS's (CFG) */
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { CONF_LANG_JAPANESE = 0, CONF_LANG_ENGLISH, CONF_LANG_GERMAN, CONF_LANG_FRENCH,
       CONF_LANG_SPANISH, CONF_LANG_ITALIAN, CONF_LANG_DUTCH, CONF_LANG_SIMP_CHINESE,
       CONF_LANG_TRAD_CHINESE, CONF_LANG_KOREAN, CONF_LANG_PORTUGUESE = 100, CONF_LANG_RUSSIAN };
enum { CONF_REGION_JP = 0, CONF_REGION_US = 1, CONF_REGION_EU = 2, CONF_REGION_KR = 4, CONF_REGION_CN = 5 };
enum { CONF_ASPECT_4_3 = 0, CONF_ASPECT_16_9 = 1 };
s32 CONF_GetLanguage(void);
s32 CONF_GetRegion(void);
static inline s32 CONF_GetAspectRatio(void) { return CONF_ASPECT_4_3; }
static inline s32 CONF_GetIdleLedMode(void) { return 0; }
#ifdef __cplusplus
}
#endif
