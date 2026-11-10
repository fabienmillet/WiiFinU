#!/bin/bash
# Run WiiFin.3dsx in Azahar (flatpak) on a virtual X display and take
# screenshots: tools/3ds/test/run.sh <seconds> [shot-every-seconds]
#   KEYS="t:key t:key ..."   xdotool keys at t seconds (Azahar's keyboard map)
#   TAPS="t:x,y ..."         stylus taps at t seconds, bottom screen pixels
#   CFG=file                 wiifin.cfg to put on the SD card
# Screenshots: tools/3ds/test/out/sNNN.png; Azahar's log: out/azahar.log
set -u
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/../../.." && pwd)
OUT=$T/out; rm -rf "$OUT"; mkdir -p "$OUT"
DUR=${1:-30}; EVERY=${2:-5}
APPDIR=$HOME/.var/app/org.azahar_emu.Azahar
SD=$APPDIR/data/azahar-emu/sdmc
mkdir -p "$SD/3ds/WiiFin"
[ -f "$SD/3ds/dspfirm.cdc" ] || head -c 4096 /dev/zero > "$SD/3ds/dspfirm.cdc"
cp "$R/WiiFin.3dsx" "$SD/3ds/WiiFin.3dsx"
if [ -n "${CFG:-}" ]; then cp "$CFG" "$SD/3ds/WiiFin/wiifin.cfg"; fi
rm -f "$SD/3ds/WiiFin/wiifin.log"
D=:97
Xvfb $D -screen 0 1100x900x24 -nolisten tcp >/dev/null 2>&1 &
XV=$!
sleep 1
DISPLAY=$D env -u WAYLAND_DISPLAY flatpak run --nosocket=wayland --socket=x11 --env=QT_QPA_PLATFORM=xcb org.azahar_emu.Azahar "$SD/3ds/WiiFin.3dsx" >"$OUT/azahar.log" 2>&1 &
AZ=$!
start=$(date +%s.%N)
el() { echo "$(date +%s.%N) - $start" | bc | awk "{printf \"%d\", \$1}"; }
win=""
n=0
declare -A done
while :; do
    ei=$(el)
    [ "$ei" -ge "$DUR" ] && break
    [ -z "$win" ] && win=$(DISPLAY=$D xdotool search --name 'Azahar' 2>/dev/null | tail -1)
    for k in ${KEYS:-}; do
        t=${k%%:*}; key=${k#*:}
        if [ -z "${done[k$k]:-}" ] && [ "$ei" -ge "$t" ]; then
            done[k$k]=1
            DISPLAY=$D xdotool keydown "$key" 2>/dev/null; sleep 0.15; DISPLAY=$D xdotool keyup "$key" 2>/dev/null
        fi
    done
    for k in ${TAPS:-}; do
        t=${k%%:*}; xy=${k#*:}
        if [ -z "${done[t$k]:-}" ] && [ "$ei" -ge "$t" ]; then
            done[t$k]=1
            # where the bottom screen is: found by tools/3ds/test/geom.txt (x0 y0 scale)
            read -r X0 Y0 S < "$T/geom.txt"
            x=${xy%,*}; y=${xy#*,}
            px=$(echo "$X0 + $x * $S" | bc); py=$(echo "$Y0 + $y * $S" | bc)
            DISPLAY=$D xdotool mousemove "${px%.*}" "${py%.*}" mousedown 1; sleep 0.12; DISPLAY=$D xdotool mouseup 1
        fi
    done
    if [ "$ei" -ge $(( n * EVERY )) ]; then
        DISPLAY=$D import -window root "$OUT/s$(printf %03d "$ei").png" 2>/dev/null
        n=$((n + 1))
    fi
    sleep 0.25
done
DISPLAY=$D import -window root "$OUT/final.png" 2>/dev/null
kill $AZ 2>/dev/null; sleep 1; flatpak kill org.azahar_emu.Azahar 2>/dev/null; kill $XV 2>/dev/null
cp "$SD/3ds/WiiFin/wiifin.log" "$OUT/wiifin.log" 2>/dev/null
ls "$OUT"
