#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Launches the game with statistics on (the installed d3d12metal.conf is left alone: the environment overrides it)
# and drives it from the profile menu into the open world (tools/game/to-gameplay.sh), screenshots land in
# build-wine/game-screens/load-N.png. The layer log is build-wine/game-logs/<name>.log.
# usage: GAME=sm1|sm2 tools/game/play.sh <name> [profile-row 1|2] [load-seconds]     (extra environment, e.g. D3D12METAL_TRACE=1, passes through)
# Refuses to start when a game is already running (it may be the user's).
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
name="${1:?usage: play.sh <name> [profile-row] [load-seconds]}"; shift
pgrep -f "$GAME_EXE" > /dev/null && { echo "$GAME_EXE is already running" >&2; exit 1; }
mkdir -p "$game_logs"
export D3D12METAL_TRACE="${D3D12METAL_TRACE:-0}" D3D12METAL_STATS="${D3D12METAL_STATS:-1}" D3D12METAL_LOG_FILE="$game_logs/$name.log"
rm -f "$D3D12METAL_LOG_FILE"
"$game_root/tools/game/launch.sh" -all "$game_logs/launch.log" > /dev/null 2>&1 &
GAME_LOG="$D3D12METAL_LOG_FILE" "$game_root/tools/game/to-gameplay.sh" "$@"
