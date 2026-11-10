# WiiFin on the Nintendo 3DS

WiiFin also runs on the **Nintendo 3DS, 2DS and New 3DS / New 2DS** (XL included), from the Homebrew Launcher. It is the same application as on the Wii, built from the same sources: profiles, home screen, libraries, search, music, subtitles, Quick Connect, the remote control from Jellyfin's dashboard.

## Install

1. A 3DS with custom firmware and the Homebrew Launcher.
2. Copy `WiiFin.3dsx` to `sdmc:/3ds/` on the SD card.
3. For sound, the DSP firmware must be on the card: `sdmc:/3ds/dspfirm.cdc` (dumped once with *DSP1*, as most 3DS homebrew need). Without it WiiFin works, silently.
4. Start **WiiFin** from the Homebrew Launcher.

Settings, profiles and the log (`wiifin.log`, for bug reports) are kept in `sdmc:/3ds/WiiFin/`. Fonts and sounds can be added there as on the Wii (`fonts/`, `sounds/`).

## The two screens

- **Bottom screen (touch)**: the menus. Touch what you want: a tap is the Wii Remote's A where you tapped. Everything can also be done with the buttons.
- **Top screen**: the video while one plays, with its subtitles; otherwise WiiFin's logo, the clock and the music playing.

## Buttons

| 3DS | Does |
|---|---|
| A / B | Select / Back |
| D-pad, Circle Pad | Move |
| Y / X | The Wii Remote's 1 / 2 (Favorite, Audio track, Search...) |
| L / R | The Wii Remote's - / + (pages, tabs, previous / next) |
| SELECT (or ZL / ZR on a New 3DS) | Zoom during a video (fit / fill), Watched, Browse... |
| START | WiiFin's HOME menu: Exit, back to the Homebrew Launcher |
| HOME | The console's HOME menu (a video pauses) |

Closing the lid pauses a video too.

## Video

Jellyfin converts every video for the 3DS: MPEG-4 at **400x240** at most (the top screen) and MP3 stereo, 350 kb/s to 1.2 Mb/s depending on *Settings > Video Quality* (Normal: 550 kb/s). Films are never played as they are (*Direct Play* is not offered): the 3DS could not decode them.

The video is decoded by the CPU (FFmpeg); the colour conversion is done by the 3DS's own hardware (Y2R) straight into the GPU's texture. On a New 3DS, WiiFin uses its faster clock and a core of its own for the video.

## Differences from the Wii version

- No screen area, smooth motion or disc slot light settings (no TV, no disc slot).
- No USB keyboard: the on-screen keyboard, by touch or with the buttons.
- The HOME menu's only choice is Exit (the console's own HOME button does the rest).
- The interface is the Wii's 640x480 one at half size: small text, sharp (drawn at the screen's own pixel size).

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
