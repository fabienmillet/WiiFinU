#!/bin/bash
# The 3DS build's requirements: devkitARM and the 3DS portlibs (dkp-pacman,
# or Fedora's pacman as on the Wii side), mbedTLS with WiiFin's config into
# libs/mbedtls/lib3ds, FFmpeg into libs/ffmpeg-3ds.  Then:
#   make -f Makefile.3ds
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${XDG_CACHE_HOME:-$HOME/.cache}/wiifin-3ds-deps"
: "${DEVKITPRO:=/opt/devkitpro}"
PACMAN=$(command -v dkp-pacman || command -v pacman)
sudo "$PACMAN" -S --noconfirm --needed 3ds-dev 3ds-freetype 3ds-libjpeg-turbo 3ds-libpng \
    3ds-libmad 3ds-zlib 3ds-bzip2 3ds-pkg-config
# 3ds-mbedtls (2.x) must not be installed: WiiFin uses its own 3.6 headers
if "$PACMAN" -Q 3ds-mbedtls >/dev/null 2>&1; then
    echo "remove 3ds-mbedtls first (its headers clash with libs/mbedtls): sudo $PACMAN -R 3ds-mbedtls"; exit 1
fi

# mbedTLS, the same version and config as the Wii's
VER=3.6.3
mkdir -p "$DEPS"
if [ ! -f "$REPO/libs/mbedtls/lib3ds/libmbedtls.a" ]; then
    cd "$DEPS"
    [ -f mbedtls-$VER.tar.bz2 ] || curl -fLO https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VER/mbedtls-$VER.tar.bz2
    rm -rf mbedtls-$VER && tar xjf mbedtls-$VER.tar.bz2
    cp "$REPO/libs/mbedtls/include/mbedtls/mbedtls_config.h" mbedtls-$VER/include/mbedtls/
    make -C mbedtls-$VER/library -j"$(nproc)" \
        CC="$DEVKITPRO/devkitARM/bin/arm-none-eabi-gcc" AR="$DEVKITPRO/devkitARM/bin/arm-none-eabi-ar" \
        CFLAGS="-O2 -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -ffunction-sections -I$DEVKITPRO/libctru/include" \
        libmbedtls.a libmbedx509.a libmbedcrypto.a
    mkdir -p "$REPO/libs/mbedtls/lib3ds"
    cp mbedtls-$VER/library/libmbed{tls,x509,crypto}.a "$REPO/libs/mbedtls/lib3ds/"
fi

[ -f "$REPO/libs/ffmpeg-3ds/lib/libavcodec.a" ] || "$REPO/tools/3ds/build_ffmpeg.sh"
echo "ready: make -f Makefile.3ds"
