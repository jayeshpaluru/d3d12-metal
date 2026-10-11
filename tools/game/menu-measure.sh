#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Measures the running game for <seconds> (default 30) without touching it: averages the layer's stats lines written
# meanwhile (D3D12METAL_STATS=1, a line every 120 presents) and prints the busiest threads' CPU% over the interval.
# usage: GAME=sm1|sm2 tools/game/menu-measure.sh [seconds] [log]   (log default: build-wine/game-logs/d3d12metal.log)
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
# The game as launch.sh starts it ("<exe> args"), not its crash handler or a shell that only mentions the name.
game_pattern="^${GAME_EXE//./\\.} "
seconds="${1:-30}"; log="${2:-$game_logs/d3d12metal.log}"
pid=$(pgrep -f "$game_pattern" | head -1)
[ -n "$pid" ] || { echo "$GAME_EXE is not running" >&2; exit 1; }
# Per-thread CPU seconds (user + system), one "index seconds" line per thread.
# (STIME and UTIME are the two fields after PRI, e.g. 46T; the first line also has USER, TT and the command.)
snap() { ps -M -p "$pid" | awk 'NR > 1 { for (p = 1; p <= NF && $p !~ /^[0-9]+[A-Z]$/; ++p) {} s = 0; for (k = p + 1; k <= p + 2; ++k) { n = split($k, a, ":"); v = 0; for (i = 1; i <= n; ++i) v = v * 60 + a[i]; s += v } print NR, s }'; }
mark=$(grep -c "stats (per frame" "$log")
a=$(snap); start=$SECONDS
sleep "$seconds"
b=$(snap); dt=$((SECONDS - start))
grep "stats (per frame" "$log" | tail -n +$((mark + 1)) | awk '
  { for (i = 1; i <= NF; i++) {
      if ($i == "fps") fps += $(i+1); if ($i == "gpu") gpu += $(i+1); if ($i == "(busy") busy += $(i+1)
      if ($i == "submits") sm += $(i+1); if ($i == "command" && $(i+1) == "buffers") cb += $(i+2)
      if ($i == "render" && $(i+1) == "passes") rp += $(i+2); if ($i == "compute" && $(i+1) == "encoders") ce += $(i+2)
      if ($i == "blit") bl += $(i+2); if ($i == "submit" && $(i-1) == "in") st += $(i+1) }
    c++ }
  END { if (c) printf "reports %d: fps %.1f  gpu %.2f ms (busy %.2f)  submits %.1f  command buffers %.1f  render passes %.1f  compute %.1f  blit %.1f  submit ms %.2f\n", c, fps/c, gpu/c, busy/c, sm/c, cb/c, rp/c, ce/c, bl/c, st/c; else print "no stats lines (D3D12METAL_STATS=1?)" }'
echo "busiest threads, CPU% over ${dt}s:"
paste <(echo "$a") <(echo "$b") | awk -v d="$dt" '$1 == $3 { printf "%.0f\n", ($4 - $2) * 100 / d }' | sort -rn | head -10 | tr '\n' ' '
echo
