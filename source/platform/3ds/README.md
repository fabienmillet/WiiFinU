# source/platform/3ds — WiiFin on the 3DS

The Wii's sources build for the 3DS as they are, with a few `#ifdef __3DS__` where the consoles differ (the start in `App.cpp`, the settings shown, the HOME menu, the transcode's size). `compat/` comes first on the include path: its `gccore.h`, `ogc/*.h`, `grrlib.h`, `wiiuse/wpad.h`, `network.h`, `asndlib.h`, `mp3player.h`... declare the libogc / GRRLIB calls WiiFin makes, and the files here implement them.

| File | What it is on the 3DS |
|---|---|
| `Gfx3ds.cpp` | GX's immediate mode (`GX_Begin` / `GX_Position` / `GX_Color` / `GX_TexCoord` / `GX_End`), its matrices and viewport, and GRRLIB, on citro3d. Primitives become triangles, batched until the texture, matrix or scissor changes. The Wii's 640x480 frame is the bottom screen at half size; the top screen has its own drawer, called at each `GRRLIB_Render`, in an 800x480 space. Textures live in linear memory (made by any thread, freed at the next frame); GX-format texels (Text's glyphs, Trickplay) are converted to the GPU's 8x8 tiles. |
| `shaders/wiifin.v.pica` | The one vertex shader: position by the matrix made on the CPU (GX projection, model-view, viewport, half size, the screen's rotation). |
| `Ogc3ds.cpp` | LWP threads and mutexes (libctru threads, priorities mapped), the time base (`gettime`, in microseconds), `SYS_Report` (debug output; `Log.cpp` wraps it as on the Wii), the console's language and region, the network (`if_config`: the system's Wi-Fi). |
| `compat/network.h` | `net_*` on BSD sockets, returning `-errno` like IOS. |
| `Input3ds.cpp` | The buttons and Circle Pad as a GameCube controller (`PAD_*`), whose names are the 3DS's; the touch screen as the Wii Remote's pointer (`WPAD_IR`), a tap as its A. |
| `Audio3ds.cpp` | ASND voices on ndsp channels (sounds copied once to linear memory), MP3Player with libmad on a thread. |
| `Player3ds.cpp` | What MPlayer does on the Wii (`WiiPlayer.h`): an HTTP(S) reader thread fills a ring buffer (the cache), FFmpeg demuxes and decodes from it; the sound (about a second ahead) is the clock, the pictures go through Y2R into one of three textures shown on the top screen, subtitles drawn over them. |
| `JpegTexture3ds.cpp` | Posters and photos into RGB565 textures, at most 512 pixels a side. |
| `Platform3ds.cpp` | The start (New 3DS speed, the GPU), the top screen when no video plays, the HOME menu / sleep hook that pauses a video. |
| `ui/` | The 3DS's own screens, 1:1 in its pixels: `Ui3ds` (spaces, touch, drawing, scrolling, the top screen's info), `Browse3ds` (home, libraries, series, page, search), `Connect3ds`, `Welcome3ds`, `Images3ds` (posters and backdrops fetched by a thread), `Job3ds`; and, included by the shared files, the 3DS drawing of the player (`PlayerView3ds.inc`), the settings (`SettingsView3ds.inc`), the music player (`MusicPlayerView3ds.inc`) and the START menu (`HomeMenu3ds.inc`): their logic stays the Wii's. |
| `Misc3ds.cpp` | Wii-only pieces with nothing to do (reload from the DOL, the music visualiser's DSP readout). |

Excluded from the 3DS build (`WII_ONLY` in `Makefile.3ds`): MPlayer's glue (`WiiPlayer.cpp`, `stream_wiifin.cpp`, `vo_wiifin.c`, `VideoSurface.cpp`, `OgcCompat.c`), `AudioSpectrum.cpp`, `JpegTexture.cpp`, `LwpJoinFix.cpp`, `Reload.cpp`.
