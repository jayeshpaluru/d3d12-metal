#!/bin/bash
# Captures the game window into build-wine/game-screens/<name>.png.
# usage: tools/game/screenshot.sh <name> [window-title-regex, default Spider]
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
name="${1:?usage: screenshot.sh <name>}"; mkdir -p "$game_screens"
id="$(swift "$game_root/tools/list_windows.swift" 2>/dev/null | grep -iE "${2:-Spider}" | head -1 | awk -F' [|] ' '{print $1}')"
[ -n "$id" ] || { echo "no window (swift tools/list_windows.swift shows the list)" >&2; exit 1; }
caffeinate -u -t 3 &
sleep 1
screencapture -x -o -l "$id" "$game_screens/$name.png" && echo "$game_screens/$name.png"
