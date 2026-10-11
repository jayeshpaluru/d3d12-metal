#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Backtraces all threads of the running game into the file given (default build-wine/game-logs/bt.txt).
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
out="${1:-$game_logs/bt.txt}"; mkdir -p "$(dirname "$out")"
pid="$(game_winedbg_pid)"; [ -n "$pid" ] || { echo "$GAME_EXE not running" >&2; exit 1; }
printf 'bt all\nquit\n' | WINEDEBUG=-all wine_steam winedbg "0x$pid" > "$out.raw" 2>&1
awk -v p="$pid" '/^Backtracing for thread/ {sub(/^0+/,"",p); on = ($0 ~ "in process 0*" p " ")} on' "$out.raw" > "$out"
rm -f "$out.raw"; wc -l "$out"
