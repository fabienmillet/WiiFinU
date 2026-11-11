#!/bin/bash
# Run WiiFin.wuhb in Cemu (Flatpak info.cemu.Cemu) on a virtual X display
# and take screenshots of the TV and the GamePad into tools/wiiu/test/out/.
#
#   tools/wiiu/test/run.sh <seconds> [every]
#
#   KEYS="10:a 14:t"      GamePad buttons at those seconds (cemu_gamepad.xml:
#                         A a, B s, X z, Y x, L q, R w, ZL e, ZR r, + m, - n,
#                         D-pad t g f h, L3 j, R3 k, HOME o)
#   TAPS="12:427,240"     touches on the GamePad (its 854x480 pixels)
#   DRAGS="12:100,200:300,200"  a finger slid from one point to the other
#   CFG=file              put a wiifin.cfg on the card first (a profile)
#   APP=WiiFin.rpx        run that instead of WiiFin.wuhb
#
# Cemu's sound is off (without an audio device it waits forever), its GPU
# is Vulkan (llvmpipe here).  Kill a run with: pkill -x cemu
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SECS=${1:-30}
EVERY=${2:-5}
OUT="$HERE/out"
APP=${APP:-$REPO/WiiFin.wuhb}
CEMU_CFG="$HOME/.var/app/info.cemu.Cemu/config/Cemu"
CEMU_DATA="$HOME/.var/app/info.cemu.Cemu/data/Cemu"
SD="$CEMU_DATA/sdcard"
DISP=:77
export DISPLAY=$DISP
unset WAYLAND_DISPLAY

rm -rf "$OUT"; mkdir -p "$OUT"
mkdir -p "$SD/wiiu/apps/WiiFin" "$CEMU_CFG/controllerProfiles"
cp "$HERE/cemu_gamepad.xml" "$CEMU_CFG/controllerProfiles/controller0.xml"
[ -n "${CFG:-}" ] && cp "$CFG" "$SD/wiiu/apps/WiiFin/wiifin.cfg"
rm -f "$SD/wiiu/apps/WiiFin/wiifin.log"

# Cemu's settings: Vulkan, no sound device, the windows where we look
S="$CEMU_CFG/settings.xml"
if [ -f "$S" ]; then
    python3 -I - "$S" <<'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r'(<Graphic>\s*<api>)\d(</api>)', r'\g<1>1\2', s)
s = re.sub(r'<TVDevice>[^<]*</TVDevice>', '<TVDevice></TVDevice>', s)
s = re.sub(r'<logflag>\d+</logflag>', '<logflag>' + str(131077 + (512 if __import__('os').environ.get('H264LOG') else 0)) + '</logflag>', s)   # files, unsupported calls, OSReport (+ H.264)
s = re.sub(r'<check_update>true</check_update>', '<check_update>false</check_update>', s)
s = re.sub(r'<open_pad>false</open_pad>', '<open_pad>true</open_pad>', s)
s = re.sub(r'<window_position>.*?</window_position>', '<window_position>\n        <x>0</x>\n        <y>0</y>\n    </window_position>', s, flags=re.S)
s = re.sub(r'<window_size>.*?</window_size>', '<window_size>\n        <x>1280</x>\n        <y>760</y>\n    </window_size>', s, flags=re.S)
s = re.sub(r'<pad_position>.*?</pad_position>', '<pad_position>\n        <x>1300</x>\n        <y>0</y>\n    </pad_position>', s, flags=re.S)
s = re.sub(r'<pad_size>.*?</pad_size>', '<pad_size>\n        <x>854</x>\n        <y>480</y>\n    </pad_size>', s, flags=re.S)
open(p, 'w').write(s)
EOF
fi

# the display, a window manager (Cemu's windows need one)
if ! pgrep -f "Xvfb $DISP " >/dev/null; then
    Xvfb $DISP -screen 0 2200x1300x24 >/dev/null 2>&1 &
    sleep 1
fi
pgrep -x openbox >/dev/null || { openbox >/dev/null 2>&1 & sleep 1; }

pkill -x cemu 2>/dev/null; sleep 1
mkdir -p "$HOME/.cache/wiiu-test"
cp "$APP" "$HOME/.cache/wiiu-test/"
RUN="$HOME/.cache/wiiu-test/$(basename "$APP")"
flatpak run --nosocket=wayland --socket=x11 info.cemu.Cemu ${CEMU_ARGS:-} -g "$RUN" > "$OUT/cemu.out" 2>&1 &
START=$(date +%s.%N)

win() { xdotool search --name "$1" 2>/dev/null | head -1; }
padwin() { xdotool search --name "^GamePad View" 2>/dev/null | head -1; }
tvwin() { xdotool search --name "^Cemu " 2>/dev/null | head -1; }

# the timed actions, sorted
ACTIONS=$(
  for k in ${KEYS:-}; do echo "${k%%:*} key ${k#*:}"; done
  for t in ${TAPS:-}; do echo "${t%%:*} tap ${t#*:}"; done
  for d in ${DRAGS:-}; do echo "${d%%:*} drag ${d#*:}"; done
  for ((s = EVERY; s <= SECS; s += EVERY)); do echo "$s shot"; done
)
ACTIONS=$(echo "$ACTIONS" | sort -n)
n=0
while read -r at what arg; do
    [ -z "$at" ] && continue
    while :; do
        now=$(echo "$(date +%s.%N) - $START" | bc)
        [ "$(echo "$now >= $at" | bc)" = 1 ] && break
        sleep 0.1
    done
    case "$what" in
    key)
        W=$(tvwin); [ -n "$W" ] && xdotool windowactivate --sync "$W" 2>/dev/null
        xdotool keydown "$arg"; sleep 0.15; xdotool keyup "$arg"
        echo "$at key $arg" >> "$OUT/actions.txt" ;;
    tap|drag)
        P=$(padwin)
        if [ -n "$P" ]; then
            eval "$(xdotool getwindowgeometry --shell "$P")"
            x1=${arg%%,*}; rest=${arg#*,}; y1=${rest%%:*}
            xdotool mousemove $((X + x1)) $((Y + y1)) mousedown 1
            if [ "$what" = drag ]; then
                to=${arg#*:}; x2=${to%%,*}; y2=${to#*,}
                for i in 1 2 3 4 5 6 7 8; do
                    xdotool mousemove $((X + x1 + (x2 - x1) * i / 8)) $((Y + y1 + (y2 - y1) * i / 8)); sleep 0.05
                done
            fi
            sleep 0.12; xdotool mouseup 1
        fi
        echo "$at $what $arg" >> "$OUT/actions.txt" ;;
    shot)
        n=$((n + 1))
        T=$(tvwin); P=$(padwin)
        [ -n "$T" ] && import -window "$T" "$OUT/tv$n.png" 2>/dev/null
        [ -n "$P" ] && import -window "$P" "$OUT/pad$n.png" 2>/dev/null ;;
    esac
done <<< "$ACTIONS"

pkill -x cemu 2>/dev/null
sleep 1
cp "$CEMU_DATA/log.txt" "$OUT/cemu-log.txt" 2>/dev/null
# WiiFin's log: Cemu writes the card's files when they are closed (never,
# killed), so it is taken from what Cemu shows of the system's output
sed -n 's/^\[[0-9:.]*\] \[OSConsole\] //p' "$OUT/cemu-log.txt" > "$OUT/wiifin.log"
echo "screenshots in $OUT ($n), log: $OUT/wiifin.log"
