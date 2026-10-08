#!/bin/bash
# Measures a fixed scene: loads a profile into the open world (tools/game/play.sh, which looks at the screen), checks that the world is
# on screen, stands still for <seconds> (default 60) with the layer's statistics on, samples the CPU use of the game and stops the game.
# Aborts (exit 1) when the game is not in gameplay before or after the sample, so a number never comes from a menu.
# Prints an averaged summary and the last reports; the full layer log is build-wine/game-logs/<name>.log.
# The API and pass profiles are off by default (they perturb the CPU use); D3D12METAL_PROFILE=1 D3D12METAL_PASS_PROFILE=1
# D3D12METAL_SUBMIT_PROFILE=1 turn them on for digging.
# usage: tools/game/measure.sh <name> [stand-seconds] [profile-row] [load-seconds]
# Refuses to start when a game is already running (it may be the user's); stops only Spider-Man.exe and its helpers.
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
name="${1:?usage: measure.sh <name> [stand-seconds] [profile-row] [load-seconds]}"
stand="${2:-60}"; row="${3:-2}"; load="${4:-120}"
export D3D12METAL_STATS=1 D3D12METAL_PROFILE="${D3D12METAL_PROFILE:-0}" D3D12METAL_PASS_PROFILE="${D3D12METAL_PASS_PROFILE:-0}"
log="$game_logs/$name.log"
stop_game() { "$game_root/tools/run-game.sh" --kill > /dev/null 2>&1 || pkill -f "$GAME_EXE"; sleep 3; }
abort() { echo "ABORT $name: $1"; stop_game; exit 1; }

"$game_root/tools/game/play.sh" "$name" "$row" "$load" > "$game_logs/$name.play.out" 2>&1 || { cat "$game_logs/$name.play.out"; abort "did not reach gameplay"; }
pid=$(pgrep -f "$GAME_EXE" | head -1)
[ -n "$pid" ] || abort "game not running"
[ "$("$game_root/tools/game/state.sh")" = gameplay ] || abort "not in gameplay after loading"
echo "standing still for ${stand}s (game pid $pid)"

# Per-thread CPU seconds (user + system) of the game, one number per thread in thread order.
thread_times() { ps -M -p "$pid" | awk 'NR > 2 { split($5, a, ":"); split($6, b, ":"); print a[1] * 60 + a[2] + b[1] * 60 + b[2] }'; }
mark=$(grep -c "stats (per frame" "$log")
t1=""; cpu_samples=()
for i in $(seq $((stand / 10))); do
    # top's second sample is the CPU use over the interval (100 = one core).
    cpu_samples+=("$(top -l 2 -s 5 -pid "$pid" -stats cpu 2>/dev/null | awk 'NF == 1 && $1 + 0 > 0 { v = $1 } END { print v }')")
    sleep 5
    [ "$i" = 2 ] && { t1="$(thread_times)"; t1_at=$SECONDS; }
    [ "$i" = "$((stand / 10 - 1))" ] && { t2="$(thread_times)"; t2_at=$SECONDS; }
done
[ "$("$game_root/tools/game/state.sh")" = gameplay ] || abort "left gameplay during the sample"
"$game_root/tools/game/screenshot.sh" "measure-$name" > /dev/null 2>&1
echo "--- stats over the sample (reports every 120 frames)"
grep "stats (per frame" "$log" | tail -n +$((mark + 1)) | awk '
  { for (i = 1; i <= NF; i++) {
      if ($i == "fps") fps += $(i+1); if ($i == "gpu") gpu += $(i+1);
      if ($i == "command" && $(i+1) == "buffers") cb += $(i+2);
      if ($i == "render" && $(i+1) == "passes") rp += $(i+2);
      if ($i == "barriers") br += $(i+1);
      if ($i == "(busy") busy += $(i+1) }
    c++ }
  END { if (c) printf "reports %d: fps %.1f  gpu %.2f ms (busy %.2f)  command buffers %.1f  render passes %.1f  barriers %.1f\n", c, fps/c, gpu/c, busy/c, cb/c, rp/c, br/c }'
grep "stats (per frame" "$log" | tail -1
echo "--- game CPU% per 10 s (top, 100 = one core): ${cpu_samples[*]}"
echo "${cpu_samples[*]}" | awk '{ for (i = 1; i <= NF; i++) s += $i; if (NF) printf "--- game CPU%% average: %.0f\n", s / NF }'
# Threads are matched by their position in the list: skipped when threads came or went between the two snapshots.
if [ "$(echo "$t1" | wc -l)" = "$(echo "$t2" | wc -l)" ]; then
    echo "--- busiest threads, CPU% over $((t2_at - t1_at)) s:"
    paste <(echo "$t1") <(echo "$t2") | awk -v dt=$((t2_at - t1_at)) '{ d = ($2 - $1) * 100 / dt; if (d >= 2) print d }' | sort -rn | awk '{ printf "%.0f ", $1 } END { print "" }'
else
    echo "--- busiest threads: thread count changed between the snapshots, skipped"
fi
for section in "api profile" "submit profile over" "pass profile over"; do
    line=$(grep -n "$section" "$log" | tail -1 | cut -d: -f1)
    [ -n "$line" ] && { echo "--- $section (last report)"; sed -n "${line},$((line+14))p" "$log"; }
done
stop_game
pgrep -f "$GAME_EXE" > /dev/null && echo "WARNING: game still running"
exit 0
