# WiiFin on the Nintendo Wii U

WiiFin also runs on the **Wii U** as a native app (not in the vWii), under the name **WiiFin U** for now (`make -f Makefile.wiiu APP_NAME=...` changes it). It is built from the same sources as the Wii version (Jellyfin client, screens, player logic, settings, translations), drawn in 16:9 at the screens' own sizes, and it uses the console's own pieces: its two screens, the GamePad's touch screen, its keyboard, and its H.264 decoder for HD video.

## Install

1. A Wii U with [Aroma](https://aroma.foryour.cafe/).
2. Copy `WiiFin.wuhb` to `sd:/wiiu/apps/` on the SD card.
3. Start **WiiFin U** from the Wii U Menu.

Settings, profiles and the log (`wiifin.log`, for bug reports) are kept in `sd:/wiiu/apps/WiiFin/`. Fonts and sounds can be added there as on the Wii (`fonts/`, `sounds/`).

## The two screens

*Settings > Screens*:

| Screens | TV | GamePad |
|---|---|---|
| **Auto** (default) | WiiFin when a TV is plugged in (HDMI) | its remote control; WiiFin itself without a TV |
| **TV** | WiiFin | its remote control |
| **GamePad** | the same as the GamePad | WiiFin, the touch screen as its pointer |

**L3 + R3** (both sticks pressed) swaps them at any time; the remote has a button to bring WiiFin to the GamePad.

The **remote control** (the GamePad while WiiFin is on the TV):
- the arrows and A, L / B / R and + (WiiFin's menu), as the GamePad's buttons;
- a **touch pad** that moves a pointer on the TV (a tap: A there);
- at the bottom, the buttons of the screen shown on the TV ("A Open", "Y Search"...), to tap;
- while a video plays: its title, a bar to tap to go somewhere, back 10 s, pause, on 30 s.

The GamePad's buttons work in every mode.

## Buttons

| GamePad, Pro Controller, Classic Controller | Does |
|---|---|
| A / B | Select / Back |
| D-pad, left stick | Move |
| Y | Search, the Wii Remote's 1 |
| X | the Wii Remote's 2 |
| L / R | Pages, tabs, previous / next (the Wii Remote's - / +) |
| ZL / ZR / - | Zoom during a video (fit / fill), Watched, Browse... |
| + | WiiFin's menu (Exit...) |
| HOME | The Wii U's HOME menu (a video pauses) |
| L3 + R3 | Swap the screens |

A **Wii Remote** works as on the Wii, its pointer included. The hints at the bottom of the screens name the controller used last.

Text (the server's address, a password, a search) is typed with the **Wii U's own keyboard** on the GamePad, one field at a time: on the sign-in screen, OK moves to the next field and A opens its keyboard (the server's address is all Quick Connect needs).

## Video

As on the Wii, a file the console can decode plays as it is (*Settings > Direct Play*), and Jellyfin converts the others.

- **H.264** is decoded by the **console's own decoder** (h264.rpl): up to 1080p, profiles Baseline, Main and High, almost no CPU. A file in MKV or MP4 goes to it through FFmpeg's Annex B filter.
- **MPEG-4 (DivX, Xvid), MPEG-1/2** are decoded by FFmpeg on the CPU (480p / 576p).
- The sound: MP3, MP2, AAC, AC-3, E-AC-3, Vorbis, Opus, FLAC, mixed down to stereo.
- The GPU draws the pictures as they come (NV12), BT.601 or BT.709, video range.

Converted videos come as **H.264 + AAC**, decoded by the console's chip, at the size of *Settings > HD Video* on the TV (**480p**, **720p** (default) or **1080p**) and always at 480p on the GamePad (854x480). If the console's decoder refuses a stream, WiiFin goes back to 480p MPEG-4 (FFmpeg, as the Wii) for the session.

| Direct play | Limits (to widen with a console) |
|---|---|
| H.264 (8-bit) | 1080p, level 4.1, 20 Mb/s (720p / 480p with *HD Video* lower, or on the GamePad) |
| MPEG-4 ASP, MPEG-1/2 | 854x576, 6 Mb/s |
| Containers | AVI, MKV/WebM, MP4/M4V/MOV, TS, MPEG-PS |

## Differences from the Wii version

- 16:9 always, the text rasterised at the screen's size (sharp on a 1080p TV); no screen area (HDMI), no smooth motion, no disc slot light settings.
- Its own player (FFmpeg and the console's H.264 decoder) in place of MPlayer CE; the music player's bars are computed from the sound played.
- Leaving goes back to the Wii U Menu (or the Homebrew Launcher, from it).
- It says "the console" where the Wii version says "the Wii"; the server's dashboard shows it as a Nintendo Wii U.

## Building

```bash
tools/wiiu/setup.sh          # devkitPPC, wut, the Wii U portlibs, mbedTLS and FFmpeg for the Wii U
make -f Makefile.wiiu        # WiiFin.wuhb (and WiiFin.rpx)
```

`setup.sh` installs `wiiu-dev` and the portlibs it uses with (dkp-)pacman (SDL2 for the sound, freetype, libpng, libjpeg-turbo, libmad...), cross-compiles mbedTLS 3.6 with WiiFin's config into `libs/mbedtls/libwiiu/` and FFmpeg into `libs/ffmpeg-wiiu/` (`tools/wiiu/build_ffmpeg.sh`). The `wiiu-mbedtls` package (pulled by `wiiu-curl`) must not be installed: its 2.x headers clash with WiiFin's.

### Testing in Cemu

`tools/wiiu/test/run.sh <seconds> [every]` runs `WiiFin.wuhb` in [Cemu](https://cemu.info) (Flatpak `info.cemu.Cemu`) on a virtual X display and takes screenshots of the TV and the GamePad into `tools/wiiu/test/out/`, with WiiFin's log (from Cemu's: Cemu writes the SD card's files only when they are closed). `KEYS="10:a 14:t"` presses buttons at those seconds (`cemu_gamepad.xml`: A `a`, B `s`, X `z`, Y `x`, L `q`, R `w`, ZL `e`, ZR `r`, + `m`, - `n`, D-pad `t g f h`, L3 `j`, R3 `k`), `TAPS="12:427,240"` touches the GamePad, `DRAGS="12:100,200:300,200"` slides on it, `TYPE="20:some.host"` types text into Cemu's keyboard (its OK is then a tap on its check key, `TAPS="30:725,395"`; letters bound to buttons, such as `a` and `s`, press them too), `CFG=file` puts a `wiifin.cfg` on the card first (a profile already signed in), `CEMU_ARGS=--force-interpreter` runs Cemu's interpreter, `SOCKLOG=1` adds Cemu's socket calls to its log.

Known with Cemu (2.6): it needs no sound device set (it waits for one forever); its recompiler loops forever on a conditional trap (`tweq`: GCC makes them from checks such as `std::shared_ptr`'s count overflow): the build uses `-fno-if-conversion` (FFmpeg's too) and fails if one is left; its PPC recompiler gets some 64-bit comparisons wrong, which breaks FFmpeg's MKV and AVI demuxers (direct play of those files: fine with `--force-interpreter`, slow); `SYSLaunchMenu` is not there (leaving takes 4 s); its keyboard is its own (no Cancel, OK only; Enter does nothing; the controller's A is its OK and B its backspace).

## How the port works

The Wii's sources are compiled as they are: `source/platform/wiiu/compat/` gives them the headers of libogc, GRRLIB and wiiuse, and the files next to it implement what they declare on the Wii U. See [source/platform/wiiu/README.md](../source/platform/wiiu/README.md).
