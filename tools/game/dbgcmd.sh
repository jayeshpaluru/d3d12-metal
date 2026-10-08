#!/bin/bash
# Runs winedbg commands against the running game: tools/game/dbgcmd.sh "info thread" "x/4gx 0x1000"
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
pid="$(game_winedbg_pid)"; [ -n "$pid" ] || { echo "$GAME_EXE not running" >&2; exit 1; }
cmds=""; for a in "$@"; do cmds="$cmds$a"$'\n'; done
printf '%squit\n' "$cmds" | WINEDEBUG=-all wine_steam winedbg "0x$pid" 2>&1
