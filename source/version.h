#pragma once

#define WIIFIN_VERSION "1.0.0"

/* who the server's dashboard says is playing (the Authorization header) */
#ifdef __3DS__
#ifndef APP_NAME
#define APP_NAME "3DSFin"
#endif
#define WIIFIN_CLIENT_NAME APP_NAME
#define WIIFIN_DEVICE_NAME "Nintendo 3DS"
#else
#define WIIFIN_CLIENT_NAME "WiiFin"
#define WIIFIN_DEVICE_NAME "Nintendo Wii"
#endif
