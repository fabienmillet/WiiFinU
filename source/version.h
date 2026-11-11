#pragma once

#define WIIFIN_VERSION "1.0.0"

/* WiiFin U's own version (WiiFin's line above stays as it is, for merges) */
#ifdef __WIIU__
#undef WIIFIN_VERSION
#define WIIFIN_VERSION "0.1.0"
#endif

/* who the server's dashboard says is playing (the Authorization header) */
#ifdef __3DS__
#ifndef APP_NAME
#define APP_NAME "3DSFin"
#endif
#define WIIFIN_CLIENT_NAME APP_NAME
#define WIIFIN_DEVICE_NAME "Nintendo 3DS"
#elif defined(__WIIU__)
#ifndef APP_NAME
#define APP_NAME "WiiFin U"
#endif
#define WIIFIN_CLIENT_NAME APP_NAME
#define WIIFIN_DEVICE_NAME "Nintendo Wii U"
#else
#define WIIFIN_CLIENT_NAME "WiiFin"
#define WIIFIN_DEVICE_NAME "Nintendo Wii"
#endif

/* its files: <WIIFIN_FILES>.cfg, .log... (the Wii U: in its folder next to
 * the apps, sd:/wiiu/apps/WiiFinU/, the .wuhb beside it) */
#ifdef __WIIU__
#ifndef APP_FILE
#define APP_FILE "wiifinu"
#endif
#define WIIFIN_FILES APP_FILE
#define APP_DIR "fs:/vol/external01/wiiu/apps/WiiFinU"
#else
#define WIIFIN_FILES "wiifin"
#endif
