#!/bin/bash
# Measures a fixed scene: loads a profile into the open world (tools/game/play.sh), stands still for <seconds> (default 60)
# with the layer's statistics, API profile and pass profile on, samples the CPU use of the game, and stops the game.
# Prints the last stats/profile reports and an averaged summary; the full layer log is build-wine/game-logs/<name>.log.
# usage: tools/game/measure.sh <name> [stand-seconds] [profile-row] [load-seconds]
# Refuses to start when a game is already running (it may be the user's); stops only Spider-Man.exe and its helpers.
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
name="${1:?usage: measure.sh <name> [stand-seconds] [profile-row] [load-seconds]}"
stand="${2:-60}"; row="${3:-2}"; load="${4:-60}"
export D3D12METAL_STATS=1 D3D12METAL_PROFILE=1 D3D12METAL_PASS_PROFILE="${D3D12METAL_PASS_PROFILE:-1}"
log="$game_logs/$name.log"
"$game_root/tools/game/play.sh" "$name" "$row" "$load" > "$game_logs/$name.play.out" 2>&1 || { cat "$game_logs/$name.play.out"; exit 1; }
pid=$(pgrep -f "$GAME_EXE" | head -1)
echo "standing still for ${stand}s (game pid $pid)"
cpu_samples=()
for i in $(seq $((stand / 5))); do
    sleep 5
    # D3D12METAL_SAMPLE=1: a 6 s stack sample of the game in the middle of the scene (build-wine/game-logs/<name>.sample.txt)
    if [ -n "${D3D12METAL_SAMPLE:-}" ] && [ "$i" = "$((stand / 10))" ]; then
        sample "$pid" 6 -file "$game_logs/$name.sample.txt" > /dev/null 2>&1
    fi
    cpu_samples+=("$(ps -o %cpu= -p "$pid" 2>/dev/null | tr -d ' ')")
done
"$game_root/tools/game/screenshot.sh" "measure-$name" > /dev/null 2>&1
# Everything between the end of loading and now is the scene; the last stand/5*... report lines are averaged.
n=$((stand / 2 / 2))   # stats lines come every 120 frames, about every 2 s
echo "--- last stats lines"
grep "stats (per frame" "$log" | tail -"$n" | awk '
  { for (i = 1; i <= NF; i++) {
      if ($i == "fps") fps += $(i+1); if ($i == "gpu") gpu += $(i+1);
      if ($i == "command" && $(i+1) == "buffers") cb += $(i+2);
      if ($i == "render" && $(i+1) == "passes") rp += $(i+2) }
    c++ }
  END { if (c) printf "averaged over %d reports: fps %.1f  gpu %.2f ms  command buffers %.1f  render passes %.1f\n", c, fps/c, gpu/c, cb/c, rp/c }'
grep "stats (per frame" "$log" | tail -2
echo "--- game CPU% samples (ps, 100 = one core): ${cpu_samples[*]}"
echo "--- api profile (last report)"
grep -n "api profile" "$log" | tail -1 | cut -d: -f1 | { read -r line; [ -n "$line" ] && sed -n "${line},$((line+15))p" "$log"; }
echo "--- pass profile (last report)"
grep -n "pass profile over" "$log" | tail -1 | cut -d: -f1 | { read -r line; [ -n "$line" ] && sed -n "${line},$((line+15))p" "$log"; }
"$game_root/tools/run-game.sh" --kill > /dev/null 2>&1 || pkill -f "$GAME_EXE"
sleep 3
pgrep -f "$GAME_EXE" > /dev/null && echo "WARNING: game still running"
