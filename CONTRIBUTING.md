# 🤝 Contributing to WiiFin U

Thank you for your interest in the project!
**WiiFin U** is a Jellyfin client for the Nintendo Wii U, the Wii U version of [WiiFin](https://github.com/fabienmillet/WiiFin).
Any help is welcome, but the project is young: some features may be incomplete or unstable on real consoles.

What it does, its screens, buttons and limits are in [docs/WIIU.md](docs/WIIU.md).

---

## 🧰 Requirements

- devkitPro with devkitPPC, wut and the `wiiu-dev` portlibs (SDL2, freetype, libpng, libjpeg, libmad...): `tools/wiiu/setup.sh` installs them with (dkp-)pacman and builds what WiiFin U bundles (mbedTLS, a small FFmpeg)
- `make` (Linux, or MSYS2 on Windows)
- The [Cemu](https://cemu.info) emulator for quick testing (`tools/wiiu/test/run.sh`)
- A real Wii U with [Aroma](https://aroma.foryour.cafe/) for final testing

---

## 📁 Project Structure

WiiFin U shares WiiFin's core and draws its own screens:

- `source/platform/wiiu/` – the Wii U itself: graphics on GX2 (both screens), input (GamePad, Pro Controller, Wii Remote, touch), sound, the video player (FFmpeg and the console's H.264 decoder), the system keyboard, and `compat/` (the libogc and GRRLIB headers the shared code uses); see its [README](source/platform/wiiu/README.md)
- `source/platform/wiiu/ui/` – the Wii U's browsing screens: home, libraries, series, a film's page, albums, search, in HD on the TV and on the GamePad
- `source/core/`, `source/jellyfin/`, `source/ui/`, `source/input/`, `source/player/` – the core shared with WiiFin: app flow, settings, Jellyfin client, playback sessions, the Wii's screens (sign-in, profiles, settings, the players), translations
- `data/` – fonts, sounds, pictures
- `lang/` – translations (gettext)
- `tools/wiiu/` – setup, FFmpeg build, test harness (Cemu)

---

## 🔄 WiiFin's changes

The shared core comes from WiiFin, kept as the `upstream` remote:

```sh
git remote add upstream https://github.com/fabienmillet/WiiFin.git
git fetch upstream develop
git merge upstream/develop
```

The Wii's own files (its Makefile, MPlayer CE, the WAD and Homebrew Channel files, the Dolphin tests) and the 3DS port ([3DSFin](https://github.com/fabienmillet/3DSFin)) are not in this repository: a merge that touches them stops on a modify/delete conflict, settled with `git rm` on those files. A change in the shared core may need its Wii U side (`source/platform/wiiu/`) too.

---

## 🧪 How to Contribute

1. **Fork** the repository and create a new branch.
2. **Make clear and atomic commits.**
3. **Build without warnings**: `make WERROR=-Werror`.
4. **Test your change in Cemu and, if you can, on a real Wii U**, with a TV and with the GamePad alone (*Settings > Screens*).
5. **Open a Pull Request** to the `main` branch.

When reporting a bug from a console, attach `sd:/wiiu/apps/WiiFinU/wiifinu.log`: it hides addresses, tokens and user ids, so it can be shared as it is.

---

## 🧭 Guidelines

- Use clear, descriptive variable names.
- Follow the existing code style and indentation.
- Platform-specific code stays in `source/platform/wiiu/`, or behind `#ifdef __WIIU__` in the shared files, so that WiiFin's changes keep merging.
- Keep both screens in mind: the TV (720p or 1080p) and the GamePad (854x480, touch); and both setups: a TV with the GamePad as the navigation, or the GamePad alone.
- The build fails if a conditional trap instruction is left (Cemu's recompiler loops forever on them): keep `-fno-if-conversion` in the flags.

---

## 📝 License

This project is licensed under **GPLv3**.
By contributing, you agree that your work will be released under this license.

---

Thanks 🙌
