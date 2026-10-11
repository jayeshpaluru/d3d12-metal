#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Stops the Spider-Man 2 process that tools/game/menu-scene.sh started (build-wine/game-logs/menu-scene.pid), and its
# crash handler, but only while it still shows a SELECT PROFILE scene: a game that left the menu may be someone playing
# it, and is left alone (exit 3). Any other instance of the game is never touched.
# usage: tools/game/menu-stop.sh
export GAME=sm2
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
pidfile="$game_logs/menu-scene.pid"
[ -f "$pidfile" ] || { echo "no $pidfile: nothing started by menu-scene.sh" >&2; exit 0; }
pid=$(sed -n 1p "$pidfile"); tag=$(sed -n 2p "$pidfile")
alive() { ps -o args= -p "$pid" 2>/dev/null | grep -q "^$GAME_EXE "; }
if ! alive; then rm -f "$pidfile"; exit 0; fi
# The newest dump (one every 500 presents) must be recent and show a known menu scene.
dumps="$game_root/build-wine/dumps"; refs="$game_root/build-wine/ref/sm2-menu"
newest=$(ls -t "$dumps/$tag"-*.png 2>/dev/null | head -1)
if [ -n "$newest" ] && [ $(( $(date +%s) - $(stat -f %m "$newest") )) -lt 120 ]; then
    scene=$("$game_root/build-wine/classify" "$newest" "$refs" | awk '{print $1}')
else
    scene=stale
fi
case "$scene" in
    unknown|stale) echo "pid $pid is not showing a known menu scene ($scene, $newest): left running, it may be in use" >&2; exit 3 ;;
esac
kill "$pid"
for _ in $(seq 20); do alive || break; sleep 1; done
alive && kill -9 "$pid"
# Its crash handler names the exe in --binary-path; only when no other instance of the game is left (started here,
# "<exe> args", or by Steam, "C:\...\<exe>").
pgrep -f '(^|\\|/)'"${GAME_EXE//./\\.}"'( |$)' > /dev/null || pkill -f "crs-handler.exe.*$GAME_EXE"
rm -f "$pidfile"
sleep 3
