#!/bin/bash
# Screenshots of the Wii U version's main screens into tools/wiiu/test/showcase/
# (run.sh with a profile signed in: CFG=<wiifinu.cfg> of a test server).
#   tools/wiiu/test/showcase.sh <wiifinu.cfg>
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
CFG0=$1
SHOW="$HERE/showcase"
mkdir -p "$SHOW"
cfg() { sed "s/^wiiu_screens=.*/wiiu_screens=$1/" "$CFG0" | { [ -n "${2:-}" ] && sed "s/^ui_sounds=1/ui_sounds=1\ndirect_play=0/" || cat; } > "$HERE/out-cfg/showcase.cfg"; }
shot() {   # <name> <seconds> <keys> [taps]
    CFG="$HERE/out-cfg/showcase.cfg" KEYS="$3" TAPS="${4:-}" "$HERE/run.sh" "$2" "$2" >/dev/null 2>&1
    cp "$HERE/out/tv1.png" "$SHOW/$1-tv.png" 2>/dev/null
    cp "$HERE/out/pad1.png" "$SHOW/$1-gamepad.png" 2>/dev/null
}
cfg 2;      shot 1-gamepad-home     26 "12:a 16:a"
cfg 1;      shot 2-tv-home          26 "12:a 16:a"
cfg 1 nd;   shot 3-tv-playing-720p  52 "12:a 16:a 24:h 25:h 26:h 27:h 30:a 36:a"
cfg 1;      shot 4-tv-settings      30 "12:g 14:a 18:g 20:a"
cfg 2;      shot 5-gamepad-keyboard 32 "12:a 16:a 24:x"
echo "in $SHOW"
