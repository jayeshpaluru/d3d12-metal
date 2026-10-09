#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Launches the game directly. Steam must already be running in its prefix; this never touches wineserver or Steam.
# usage: GAME=sm1|sm2 tools/game/launch.sh <WINEDEBUG> <logfile> [game args]   (game args default to the profile's: -nolauncher)
# Stop the game with tools/run-game.sh --kill (kills only the game by exe name).
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
[ -n "${LAUNCH_DEBUG:-}" ] && set -x
dbg="${1:?usage: launch.sh <WINEDEBUG> <logfile> [game args]}"; out="${2:?logfile}"; shift 2
# shellcheck disable=SC2086
[ $# -eq 0 ] && set -- $GAME_ARGS
export SteamAppId="$GAME_APPID" SteamGameId="$GAME_APPID"
# Rosetta hides AVX/AVX2/F16C from x86 code unless asked; Spider-Man 2 requires AVX2 and F16C (harmless for sm1).
export ROSETTA_ADVERTISE_AVX=1
[ -d "$GAME_FOLDER" ] || { echo "$GAME_FOLDER does not exist (GAME=$GAME; still downloading? set GAME_FOLDER)" >&2; exit 1; }
out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"  # absolute: we change directory next
cd "$GAME_FOLDER" || exit 1
[ -f "$GAME_EXE" ] || { echo "$GAME_EXE not found in $GAME_FOLDER" >&2; exit 1; }
WINEDEBUG="$dbg" WINEPREFIX="$STEAM_WINEPREFIX" exec "$WINE_ROOT/bin/wine" "$GAME_EXE" "$@" > "$out" 2>&1
