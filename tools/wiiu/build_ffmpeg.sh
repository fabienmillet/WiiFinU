#!/bin/bash
# Cross-compile FFmpeg (H.264 / MPEG-4 / MPEG-2 video, AAC / MP3 / AC-3 /
# Opus / Vorbis / FLAC audio, TS / MP4 / MKV / AVI demuxers, swscale, the
# H.264 to Annex B filter for the console's decoder) for the
# Wii U (devkitPPC + wut) into libs/ffmpeg-wiiu.  Usage: tools/wiiu/build_ffmpeg.sh
# FFmpeg's source is the one the 3DS build uses (same cache).
set -e
VER=7.1
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${XDG_CACHE_HOME:-$HOME/.cache}/wiifin-3ds-deps"
OUT="$REPO/libs/ffmpeg-wiiu"
: "${DEVKITPRO:=/opt/devkitpro}"
WUT="$DEVKITPRO/wut"
export PATH="$DEVKITPRO/devkitPPC/bin:$PATH"
mkdir -p "$DEPS"
cd "$DEPS"
[ -d ffmpeg-$VER ] || { [ -f ffmpeg-$VER.tar.xz ] || curl -fLO https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz; tar xJf ffmpeg-$VER.tar.xz; }
rm -rf ffmpeg-build-wiiu && mkdir ffmpeg-build-wiiu && cd ffmpeg-build-wiiu
../ffmpeg-$VER/configure --prefix="$OUT" \
  --enable-cross-compile --cross-prefix=powerpc-eabi- --arch=ppc --cpu=750 --target-os=none \
  --extra-cflags="-DESPRESSO -mcpu=750 -meabi -mhard-float -D__WIIU__ -D__WUT__ -I$WUT/include -ffunction-sections -fdata-sections -fno-if-conversion -fno-if-conversion2 -Wno-incompatible-pointer-types" \
  --extra-ldflags="-specs=$WUT/share/wut.specs -L$WUT/lib" --extra-libs="-lwut" \
  --disable-programs --disable-doc --disable-debug --disable-network --disable-everything \
  --disable-avdevice --disable-avfilter --disable-postproc --disable-iconv \
  --enable-swresample --enable-swscale --enable-pthreads --disable-runtime-cpudetect --disable-altivec \
  --enable-decoder=h264,mpeg4,mpeg2video,mpeg1video,aac,aac_latm,mp3,mp3float,mp2,ac3,eac3,opus,vorbis,flac \
  --enable-parser=h264,mpeg4video,mpegvideo,aac,aac_latm,mpegaudio,ac3,opus,vorbis,flac \
  --enable-demuxer=mpegts,mov,matroska,avi,mp3,aac,h264,mpegps,ac3,flac,ogg \
  --enable-bsf=h264_mp4toannexb,extract_extradata \
  --pkg-config=false
make -j"$(nproc)"
make install
echo "FFmpeg installed into $OUT"
