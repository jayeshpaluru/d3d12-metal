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
# tools/game/menu-stop.sh, which stops only the process started here (its pid is in build-wine/game-logs/menu-scene.pid)
# and only while it still shows the menu: someone may have picked the game up and started playing.
# usage: tools/game/menu-scene.sh <tag> [scene|any] [attempts]
# Refuses to start while the game runs (it may be the user's).
export GAME=sm2
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
# The game as Steam or launch.sh starts it ("C:\...\<exe>" or "<exe> args"), not a shell that only mentions the name.
game_pattern='(^|\\|/)'"${GAME_EXE//./\\.}"'( |$)'
tag="${1:?usage: menu-scene.sh <tag> [scene|any] [attempts]}"; want="${2:-any}"; attempts="${3:-3}"
dumps="$game_root/build-wine/dumps"; refs="$game_root/build-wine/ref/sm2-menu"; log="$game_logs/d3d12metal.log"
classify="$game_root/build-wine/classify"
[ -x "$classify" ] || swiftc -O -o "$classify" "$game_root/tools/game/classify.swift" || exit 1
ls "$refs"/*.png > /dev/null 2>&1 || { echo "no menu references in $refs (see the header)" >&2; exit 2; }
mkdir -p "$dumps"
pgrep -f "$game_pattern" > /dev/null && { echo "$GAME_EXE is already running; not starting another" >&2; exit 2; }
pidfile="$game_logs/menu-scene.pid"
for i in $(seq 1 "$attempts"); do
    rm -f "$dumps/$tag"-*.png "$pidfile"
    : > "$log"
    D3D12METAL_DUMP_PRESENT="$dumps/$tag.png" D3D12METAL_DUMP_PRESENT_FRAME=2000 D3D12METAL_DUMP_PRESENT_EVERY=500 \
        D3D12METAL_STATS=1 nohup "$game_root/tools/game/launch.sh" -all "$game_logs/$tag.out" > /dev/null 2>&1 &
    # launch.sh execs wine, which becomes the game: the same pid.
    echo "$!" > "$pidfile"
    echo "$tag" >> "$pidfile"
    end=$((SECONDS + 900)); scene=none; checked=""
    while [ $SECONDS -lt $end ]; do
        sleep 10
        pgrep -f "$game_pattern" > /dev/null || { echo "attempt $i: the game exited"; break; }
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
    "$game_root/tools/game/menu-stop.sh" || exit 1
done
exit 1
