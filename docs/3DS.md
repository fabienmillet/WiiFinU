# WiiFin on the Nintendo 3DS

WiiFin also runs on the **Nintendo 3DS, 2DS and New 3DS / New 2DS** (XL included), from the Homebrew Launcher, under a name of its own (**3DSFin** for now: `make -f Makefile.3ds APP_NAME=...` changes it everywhere). It is built from the same sources as the Wii version (Jellyfin client, player logic, settings, translations), with screens of its own made for the two screens and the stylus: profiles, sign-in (the 3DS's keyboard, Quick Connect, servers found on the network), home rows, libraries, series, a film's page, search, the player, music, settings.

## Install

1. A 3DS with custom firmware and the Homebrew Launcher.
2. Copy `WiiFin.3dsx` to `sdmc:/3ds/` on the SD card.
3. For sound, the DSP firmware must be on the card: `sdmc:/3ds/dspfirm.cdc` (dumped once with *DSP1*, as most 3DS homebrew need). Without it WiiFin works, silently.
4. Start **WiiFin** from the Homebrew Launcher.

Settings, profiles and the log (`wiifin.log`, for bug reports) are kept in `sdmc:/3ds/WiiFin/`. Fonts and sounds can be added there as on the Wii (`fonts/`, `sounds/`).

## The two screens

- **Bottom screen (touch)**: browsing and the controls, drawn at the screen's own size: tap, drag the lists and rows, tap the bar's back arrow, search, settings. Everything can also be done with the buttons.
- **Top screen**: what is selected (its backdrop or poster, title, year, length, synopsis), the cover and title of the music playing, or the video and its subtitles while one plays.

## Buttons

| 3DS | Does |
|---|---|
| A / B | Select / Back |
| D-pad, Circle Pad | Move |
| Y | Search (the 3DS's keyboard) |
| X | Remove a profile (pressed twice), shuffle in the music player |
| L / R | The Wii Remote's - / + (pages, tabs, previous / next) |
| SELECT (or ZL / ZR on a New 3DS) | Zoom during a video (fit / fill), Watched, Browse... |
| START | WiiFin's HOME menu: Exit, back to the Homebrew Launcher |
| HOME | The console's HOME menu (a video pauses) |

Closing the lid pauses a video too.

## Video

As on the Wii, a file the console can decode plays as it is (*Settings > Direct Play*, Auto by default), and Jellyfin converts the others: MPEG-4 at **400x240** at most (the top screen) and MP3 stereo, 350 kb/s to 1.2 Mb/s depending on *Settings > Video Quality* (Normal: 550 kb/s).

| Direct play | 3DS / 2DS | New 3DS / New 2DS |
|---|---|---|
| MPEG-4 ASP (DivX, Xvid) | ≤ 400x240, ≤ 1 Mb/s | ≤ 640x480, ≤ 1.5 Mb/s |
| H.264 (8-bit) | ≤ 400x240, level ≤ 3.0, ≤ 0.8 Mb/s | ≤ 640x360, level ≤ 3.1, ≤ 1.2 Mb/s |
| Containers | AVI, MKV/WebM, MP4/M4V/MOV, TS | same |
| Audio | MP3, MP2, AAC; 2 channels at most | same |

Always 30 fps at most, no subtitles chosen (they are burned in by a conversion), and the whole file under 2.5 Mb/s and the link measured before playing. A file that turns out too heavy (frames dropped over 10 s) goes on converted. These limits are cautious, measured in Azahar only (New 3DS: an Xvid 640x360 at 1 Mb/s takes 30% of a core, an H.264 one 40%): to widen with real consoles. Not decoded at all: HEVC, VP8/VP9, AV1, MPEG-1/2, AC3, DTS, Vorbis, FLAC.

The video is decoded by the CPU (FFmpeg); the colour conversion is done by the 3DS's own hardware (Y2R) straight into the GPU's texture. On a New 3DS, WiiFin uses its faster clock and a core of its own for the video.

## Differences from the Wii version

- Its own screens (source/platform/3ds/ui): two screens, the stylus, text at the screen's own size.
- No screen area, smooth motion, disc slot light, home layout or library view settings.
- Text is typed with the 3DS's keyboard. START's menu only leaves (the console's HOME button is the system's).
- It says "the console" where the Wii version says "the Wii"; the server's dashboard shows it as a Nintendo 3DS.

## Building

```bash
tools/3ds/setup.sh          # devkitARM, 3DS portlibs, mbedTLS and FFmpeg for the 3DS
make -f Makefile.3ds        # WiiFin.3dsx
```

`setup.sh` installs `3ds-dev` and the portlibs with (dkp-)pacman, cross-compiles mbedTLS 3.6 with WiiFin's config into `libs/mbedtls/lib3ds/` and a small FFmpeg (H.264, MPEG-4, AAC, MP3 decoders; TS, MP4, MKV, AVI demuxers) into `libs/ffmpeg-3ds/` (`tools/3ds/build_ffmpeg.sh`). The `3ds-mbedtls` package must not be installed: its 2.x headers clash with WiiFin's.

### Testing in Azahar

`tools/3ds/test/run.sh <seconds> [every]` runs `WiiFin.3dsx` in [Azahar](https://azahar-emu.org) (Flatpak) on a virtual X display and takes screenshots into `tools/3ds/test/out/`, with the log WiiFin wrote. `KEYS="10:a 14:a"` presses buttons at those seconds (Azahar's keyboard map: A `a`, B `s`, X `z`, Y `x`, D-pad `t g f h`, L `q`, R `w`, START `m`, SELECT `n`), `TAPS="12:160,120"` taps the touch screen, `CFG=file` puts a `wiifin.cfg` on the card first (a profile already signed in). Azahar must use OpenGL (its software renderer runs at a few percent of real time).

## How the port works

The Wii's sources are compiled as they are: `source/platform/3ds/compat/` gives them the headers of libogc, GRRLIB and wiiuse, and the files next to it implement what they declare on the 3DS. See [source/platform/3ds/README.md](../source/platform/3ds/README.md).
