#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Launches Spider-Man 2 to its SELECT PROFILE menu and leaves it running there, without sending any input (the game
# shows the menu by itself). The 3D scene behind the menu depends on the saves (seen: a sunset with Peter and Miles,
# a night scene, Spider-Man on a rooftop by day) and may vary between launches; <scene> picks one by the name of its
# reference and relaunches until it shows (any: the first menu seen).
# Detection: every 500th present from 2000 is dumped into build-wine/dumps/<tag>-N.png and compared (build-wine/classify)
# with references build-wine/ref/sm2-menu/<scene>.png, <scene>.<n>.png, made locally from such dumps
# (`build-wine/classify --save build-wine/dumps/<tag>-N.png build-wine/ref/sm2-menu/<scene>.png`; never committed).
# Extra D3D12METAL_* variables pass through (D3D12METAL_STATS=1 is set). The layer log is build-wine/game-logs/d3d12metal.log
# (the installed conf's log_file; truncated at each attempt). Measure with tools/game/menu-measure.sh, stop with
# GAME=sm2 tools/run-game.sh --kill.
# usage: tools/game/menu-scene.sh <tag> [scene|any] [attempts]
# Refuses to start while the game runs (it may be the user's).
export GAME=sm2
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
tag="${1:?usage: menu-scene.sh <tag> [scene|any] [attempts]}"; want="${2:-any}"; attempts="${3:-3}"
dumps="$game_root/build-wine/dumps"; refs="$game_root/build-wine/ref/sm2-menu"; log="$game_logs/d3d12metal.log"
classify="$game_root/build-wine/classify"
[ -x "$classify" ] || swiftc -O -o "$classify" "$game_root/tools/game/classify.swift" || exit 1
ls "$refs"/*.png > /dev/null 2>&1 || { echo "no menu references in $refs (see the header)" >&2; exit 2; }
mkdir -p "$dumps"
pgrep -f "$GAME_EXE" > /dev/null && { echo "$GAME_EXE is already running; not starting another" >&2; exit 2; }
stop() {
    "$game_root/tools/run-game.sh" --kill > /dev/null 2>&1
    pkill -f crs-handler.exe
    while pgrep -f "$GAME_EXE" > /dev/null; do sleep 2; done
    sleep 3
}
for i in $(seq 1 "$attempts"); do
    rm -f "$dumps/$tag"-*.png
    : > "$log"
    D3D12METAL_DUMP_PRESENT="$dumps/$tag.png" D3D12METAL_DUMP_PRESENT_FRAME=2000 D3D12METAL_DUMP_PRESENT_EVERY=500 \
        D3D12METAL_STATS=1 nohup "$game_root/tools/game/launch.sh" -all "$game_logs/$tag.out" > /dev/null 2>&1 &
    end=$((SECONDS + 900)); scene=none; checked=""
    while [ $SECONDS -lt $end ]; do
        sleep 10
        pgrep -f "$GAME_EXE" > /dev/null || { echo "attempt $i: the game exited"; break; }
        newest=$(ls -t "$dumps/$tag"-*.png 2>/dev/null | head -1)
        [ -n "$newest" ] && [ "$newest" != "$checked" ] || continue
        checked="$newest"
        scene=$("$classify" "$newest" "$refs" | awk '{print $1}')
        [ "$scene" != unknown ] && break
    done
    echo "attempt $i: $scene ($checked)"
    if [ "$scene" != none ] && [ "$scene" != unknown ] && { [ "$want" = any ] || [ "$want" = "$scene" ]; }; then
        sleep 20   # let the scene settle
        echo "$scene"
        exit 0
    fi
    stop
done
exit 1
