#!/bin/bash
# Drives a freshly launched game from the profile menu into the open world (tools/game/keys.sh, no macOS permissions needed):
# waits for the window, picks the profile (default 2: the second row), confirms CONTINUE and takes a screenshot every 10 s
# while it loads. usage: GAME_LOG=<layer log> tools/game/to-gameplay.sh [profile-row 1|2] [load-seconds]
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
row="${1:-2}"; load="${2:-60}"
tools="$game_root/tools/game"
# The menu is up once the layer has printed a stats line (launch with D3D12METAL_STATS=1 D3D12METAL_LOG_FILE=<file>, pass the file in GAME_LOG).
log="${GAME_LOG:?set GAME_LOG to the layer log file the game was launched with}"
# (the splash screens print stats lines too; the profile menu is the first with 60+ render passes per frame)
for _ in $(seq 180); do
    tail -1 "$log" 2>/dev/null | grep -q "render passes [6-9][0-9]\." && break; sleep 2
done
sleep 5
steps=(); for _ in $(seq $((row - 1))); do steps+=(down); done
"$tools/keys.sh" "${steps[@]}" enter wait:4000 enter   # profile, then CONTINUE on the main menu
for i in $(seq $((load / 10))); do sleep 10; "$tools/screenshot.sh" "load-$i" >/dev/null; done
# Gameplay only reads the keyboard once the window has been clicked (menus take keys without it).
"$tools/keys.sh" click:500:400 wait:500
