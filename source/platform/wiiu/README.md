# source/platform/wiiu — WiiFin on the Wii U

The Wii's sources build for the Wii U as they are, with a few `#ifdef __WIIU__` where the consoles differ (the start in `main.cpp` / `App.cpp`, the settings shown, the button names, the transcode's size, the keyboard). `compat/` comes first on the include path: its `gccore.h`, `ogc/*.h`, `grrlib.h`, `wiiuse/wpad.h`, `network.h`, `asndlib.h`, `mp3player.h`... declare the libogc / GRRLIB calls WiiFin makes, and the files here implement them with wut.

| File | What it is on the Wii U |
|---|---|
| `GfxWiiU.cpp` | GX's immediate mode, its matrices and viewport, and GRRLIB, on GX2 (WHBGfx). A frame is recorded (vertices moved on the CPU into the Wii's 640x480 frame, batches by texture and scissor), then drawn on the TV and the GamePad, stretched to each (Ui widens the frame to 16:9: 854x480 units). One shader for everything (SDL2's texture shader, a white texel for colours), Moonlight's NV12 shader for the video (its coefficients patched to BT.601 / BT.709, two blended quads for video range). Remote mode: the GamePad's own frame (PlatformWiiU's drawer). ProcUI through WHBProc at each `GRRLIB_Render`. |
| `shaders/` | The two shaders' binaries (assembled with latte-assembler by SDL2's and Moonlight's ports) and their sources. |
| `OgcWiiU.cpp` | LWP threads and mutexes on coreinit's, the time base, `SYS_Report` (OSReport), the console's language, region and serial number (the device id), the network's address (nn::ac). |
| `InputWiiU.cpp`, `WpadWiiU.cpp` | The GamePad (VPAD), Pro and Classic Controllers (KPAD) as a GameCube controller (the 3DS's mapping: Y "1", X "2", L/R "-"/"+", + the menu, ZL/ZR/- the zoom), a Wii Remote as itself (its pointer), the touch screen as the pointer in Mirror mode (a tap is A, or the button of a hint tapped). |
| `AudioWiiU.cpp` | One output (SDL2's audio, 48 kHz): ASND's voices, MP3Player (libmad on a thread) and the player's stream mixed in software; the stream's clock. |
| `PlayerWiiU.cpp` | What MPlayer does on the Wii (`WiiPlayer.h`), as the 3DS's player: an HTTP(S) reader thread fills a ring buffer, FFmpeg demuxes and decodes on cores 0 and 2 (the menus have core 1); pictures as NV12 textures, the sound is the clock. |
| `H264WiiU.cpp` | The console's H.264 decoder (h264.rpl): a pool of NV12 frames it writes, drawn by the GPU as they are. |
| `SwkbdWiiU.cpp` | The system's keyboard (nn::swkbd) for `Keyboard`'s fields. |
| `PlatformWiiU.cpp` | The start, the screens' mode (Auto / TV / GamePad, L3 + R3), the GamePad's remote control, the HOME menu pausing a video. |
| `JpegTextureWiiU.cpp`, `MiscWiiU.cpp` | Posters and photos into RGBA8 textures (1024 pixels a side at most); the music player's bars from the sound played; no reload from a DOL. |
| `meta/` | The icon and the TV / GamePad splash screens of the `.wuhb`. |

Excluded from the Wii U build (`WII_ONLY` in `Makefile.wiiu`): MPlayer's glue (`WiiPlayer.cpp`, `stream_wiifin.cpp`, `vo_wiifin.c`, `VideoSurface.cpp`, `OgcCompat.c`), `AudioSpectrum.cpp`, `JpegTexture.cpp`, `LwpJoinFix.cpp`, `Reload.cpp`.

Credits: the shaders come from [SDL2's Wii U port](https://github.com/devkitPro/SDL) (zlib licence) and [Moonlight for Wii U](https://github.com/GaryOderNichts/moonlight-wiiu) (GPL-3.0), whose use of the H.264 decoder and of the keyboard this port follows.
