#!/bin/bash
# Cross-compile a small FFmpeg (H.264 / AAC / MP3 decoders, TS / MP4 / MKV
# demuxers) for the 3DS into libs/ffmpeg-3ds.  Usage: tools/3ds/build_ffmpeg.sh
set -e
VER=7.1
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${XDG_CACHE_HOME:-$HOME/.cache}/wiifin-3ds-deps"
OUT="$REPO/libs/ffmpeg-3ds"
: "${DEVKITPRO:=/opt/devkitpro}"
export PATH="$DEVKITPRO/devkitARM/bin:$PATH"
mkdir -p "$DEPS"
cd "$DEPS"
[ -d ffmpeg-$VER ] || { curl -fLO https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz; tar xJf ffmpeg-$VER.tar.xz; }
rm -rf ffmpeg-build && mkdir ffmpeg-build && cd ffmpeg-build
../ffmpeg-$VER/configure --prefix="$OUT" \
  --enable-cross-compile --cross-prefix=arm-none-eabi- --arch=arm --cpu=armv6k --target-os=none \
  --extra-cflags="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -I$DEVKITPRO/libctru/include -ffunction-sections -fdata-sections -Wno-incompatible-pointer-types" \
  --extra-ldflags="-march=armv6k -mfloat-abi=hard -mtp=soft -specs=3dsx.specs -L$DEVKITPRO/libctru/lib" --extra-libs="-lctru" \
  --disable-programs --disable-doc --disable-debug --disable-network --disable-everything \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc --disable-iconv \
  --enable-swresample --enable-pthreads --disable-runtime-cpudetect \
  --enable-armv5te --enable-armv6 --enable-vfp --disable-neon --disable-armv6t2 --disable-vfpv3 \
  --enable-decoder=h264,mpeg4,aac,mp3,mp3float,mp2 \
  --enable-parser=h264,mpeg4video,aac,aac_latm,mpegaudio \
  --enable-demuxer=mpegts,mov,matroska,avi,mp3,aac,h264 \
  --enable-small --pkg-config=false
make -j"$(nproc)"
make install
echo "FFmpeg installed into $OUT"
