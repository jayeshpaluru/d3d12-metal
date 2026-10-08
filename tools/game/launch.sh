#!/bin/bash
# Launches the game directly. Steam must already be running in its prefix; this never touches wineserver or Steam.
# usage: tools/game/launch.sh <WINEDEBUG> <logfile> [game args]   (game args default to -nolauncher)
# Stop the game with tools/run-game.sh --kill (kills only the game by exe name).
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
[ -n "${LAUNCH_DEBUG:-}" ] && set -x
dbg="${1:?usage: launch.sh <WINEDEBUG> <logfile> [game args]}"; out="${2:?logfile}"; shift 2
[ $# -eq 0 ] && set -- -nolauncher
export SteamAppId="$GAME_APPID" SteamGameId="$GAME_APPID"
out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"  # absolute: we change directory next
cd "$GAME_FOLDER" || exit 1
WINEDEBUG="$dbg" WINEPREFIX="$STEAM_WINEPREFIX" exec "$WINE_ROOT/bin/wine" "$GAME_EXE" "$@" > "$out" 2>&1
