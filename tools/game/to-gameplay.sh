#!/bin/bash
# Drives a freshly launched game from the profile menu into the open world (tools/game/keys.sh, no macOS permissions needed):
# waits for the window, picks the profile (default 2: the second row), confirms CONTINUE and takes a screenshot every 10 s
# while it loads. usage: tools/game/to-gameplay.sh [profile-row 1|2] [load-seconds]
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
row="${1:-2}"; load="${2:-60}"
tools="$game_root/tools/game"
for _ in $(seq 120); do "$tools/keys.sh" list 2>/dev/null | grep -q "Spider-Man" && break; sleep 2; done
sleep 20   # the menu needs a moment before it takes input
steps=(); for _ in $(seq $((row - 1))); do steps+=(down); done
"$tools/keys.sh" "${steps[@]}" enter wait:4000 enter   # profile, then CONTINUE on the main menu
for i in $(seq $((load / 10))); do sleep 10; "$tools/screenshot.sh" "load-$i" >/dev/null; done
