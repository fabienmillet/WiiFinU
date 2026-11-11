#!/bin/bash
# The Wii U build's requirements: devkitPPC, wut and the portlibs it uses
# (dkp-pacman, or Fedora's pacman as on the Wii side), mbedTLS with WiiFin's
# config into libs/mbedtls/libwiiu, FFmpeg into libs/ffmpeg-wiiu.  Then:
#   make
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${XDG_CACHE_HOME:-$HOME/.cache}/wiifin-3ds-deps"
: "${DEVKITPRO:=/opt/devkitpro}"
PACMAN=$(command -v dkp-pacman || command -v pacman)
SUDO=sudo; [ "$(id -u)" = 0 ] && SUDO=   # (root in a CI container)
$SUDO "$PACMAN" -S --noconfirm --needed wiiu-dev ppc-freetype ppc-libjpeg-turbo ppc-libpng ppc-zlib \
    ppc-bzip2 ppc-pkg-config
# wiiu-mbedtls (2.x, pulled by wiiu-curl) must not be installed: WiiFin uses its own 3.6 headers
if "$PACMAN" -Q wiiu-mbedtls >/dev/null 2>&1; then
    echo "remove wiiu-mbedtls first (its headers clash with libs/mbedtls): $SUDO $PACMAN -R wiiu-curl wiiu-mbedtls"; exit 1
fi

# mbedTLS, the same version and config as the Wii's
VER=3.6.3
mkdir -p "$DEPS"
if [ ! -f "$REPO/libs/mbedtls/libwiiu/libmbedtls.a" ]; then
    cd "$DEPS"
    [ -f mbedtls-$VER.tar.bz2 ] || curl -fLO https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VER/mbedtls-$VER.tar.bz2
    [ -d mbedtls-$VER ] || tar xjf mbedtls-$VER.tar.bz2
    cp "$REPO/libs/mbedtls/include/mbedtls/mbedtls_config.h" mbedtls-$VER/include/mbedtls/
    make -C mbedtls-$VER/library clean >/dev/null
    make -C mbedtls-$VER/library -j"$(nproc)" \
        CC="$DEVKITPRO/devkitPPC/bin/powerpc-eabi-gcc" AR="$DEVKITPRO/devkitPPC/bin/powerpc-eabi-ar" \
        CFLAGS="-O2 -DESPRESSO -mcpu=750 -meabi -mhard-float -D__WIIU__ -D__WUT__ -ffunction-sections -I$DEVKITPRO/wut/include" \
        libmbedtls.a libmbedx509.a libmbedcrypto.a
    mkdir -p "$REPO/libs/mbedtls/libwiiu"
    cp mbedtls-$VER/library/libmbed{tls,x509,crypto}.a "$REPO/libs/mbedtls/libwiiu/"
    make -C mbedtls-$VER/library clean >/dev/null
fi

[ -n "$SKIP_FFMPEG" ] || [ -f "$REPO/libs/ffmpeg-wiiu/lib/libavcodec.a" ] || "$REPO/tools/wiiu/build_ffmpeg.sh"
echo "ready: make"
