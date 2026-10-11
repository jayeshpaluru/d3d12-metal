#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# A/B of layer settings on Spider-Man 2's SELECT PROFILE scenes. For each round and each variant (a quoted list of
# VAR=value settings, "" for none) launches the game to the menu (tools/game/menu-scene.sh, any scene), measures it
# twice (tools/game/menu-measure.sh) and stops it (tools/game/menu-stop.sh). The scene differs between launches, so
# every result line names it. A result is kept only when the game still showed the menu when it was stopped; when it
# did not (someone may be playing it), the game is left running and the A/B ends.
# usage: tools/game/menu-ab.sh <rounds> <seconds> <variant>...
#   e.g. tools/game/menu-ab.sh 3 30 "" "D3D12METAL_ASYNC_SUBMIT=0" "GAME_OUT=/path/to/other/build-wine/out"
# Results: one line per measurement on stdout and in build-wine/game-logs/menu-ab.txt.
export GAME=sm2
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
rounds="${1:?usage: menu-ab.sh <rounds> <seconds> <variant>...}"; seconds="${2:?seconds}"; shift 2
out="$game_logs/menu-ab.txt"
for round in $(seq 1 "$rounds"); do
    for variant in "$@"; do
        # Each variant installs its build first: GAME_OUT=<dir> in a variant A/Bs two builds (the conf is kept).
        env $variant "$game_root/tools/install-game.sh" > /dev/null || exit 1
        if ! scene=$(env $variant "$game_root/tools/game/menu-scene.sh" ab any 2); then
            echo "round $round [${variant:-default}]: no menu (${scene//$'\n'/; }); the A/B ends" | tee -a "$out"
            exit 1
        fi
        scene=$(echo "$scene" | tail -1)
        lines=()
        for _ in 1 2; do
            lines+=("round $round [${variant:-default}] $scene: $("$game_root/tools/game/menu-measure.sh" "$seconds" | head -1)")
        done
        if ! "$game_root/tools/game/menu-stop.sh"; then
            echo "round $round [${variant:-default}]: the game left the menu; results discarded, game left running" | tee -a "$out"
            exit 3
        fi
        printf '%s\n' "${lines[@]}" | tee -a "$out"
    done
done
