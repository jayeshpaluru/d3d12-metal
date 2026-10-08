#!/bin/bash
# Runs the Godot 4 test project (tests/godot/project) under Wine through our d3d12/dxgi DLLs and
# validates the screenshot it saves (tools/check_godot_screenshot.py). Needs build-wine/out
# (tools/build-wine.sh) and the official Windows build of Godot 4.7.2 (untrusted: run under Wine only):
#   gh release download 4.7.2-stable -R godotengine/godot -p 'Godot_v4.7.2-stable_win64.exe.zip' -D $GODOT_DIR
#   unzip $GODOT_DIR/Godot_v4.7.2-stable_win64.exe.zip -d $GODOT_DIR
#
# (The console launcher exe hangs at exit under Wine; the GUI exe still writes stdout.)
# usage: tools/run-godot-test.sh [--method forward_plus|mobile] [--frames N] [--stats]
# Prints PASS/FAIL and the frame rate; the screenshot is build-wine/screens/godot-<method>.png.
#
# Environment (defaults in brackets): WINE_ROOT, WINEPREFIX (as run-wine-tests.sh), GODOT_DIR [deps/godot].
# D3D12METAL_LOG=1 traces every bridge call, D3D12METAL_STATS=1 prints per-frame counters.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/build-wine/out"
screens="$root/build-wine/screens"
logs="$root/build-wine/logs"
WINE_ROOT="${WINE_ROOT:-/Users/jsp/code/deps/wine/Wine Devel.app/Contents/Resources/wine}"
export WINEPREFIX="${WINEPREFIX:-/Users/jsp/code/deps/wineprefix}"
godot_dir="${GODOT_DIR:-/Users/jsp/code/deps/godot}"
exe="$godot_dir/Godot_v4.7.2-stable_win64.exe"
method=forward_plus
frames=240
while [ $# -gt 0 ]; do
    case "$1" in
        --method) method="$2"; shift 2 ;;
        --frames) frames="$2"; shift 2 ;;
        --stats) export D3D12METAL_STATS=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

for path in "$out/d3d12.dll" "$out/dxgi.dll" "$out/x86_64-unix/d3d12metal.so" "$exe" "$WINE_ROOT/bin/wine"; do
    [ -e "$path" ] || { echo "missing $path" >&2; exit 2; }
done
mkdir -p "$screens" "$logs"
shot="$screens/godot-$method.png"
log="$logs/godot-$method.log"
rm -f "$shot"

# Godot loads d3d12.dll/dxgi.dll from its own directory; copy ours (and the unix half, found next to d3d12.dll) there.
mkdir -p "$godot_dir/x86_64-unix"
cp "$out/d3d12.dll" "$out/dxgi.dll" "$godot_dir/"
cp "$out/x86_64-unix/d3d12metal.so" "$godot_dir/x86_64-unix/"
export WINEDLLPATH="$out"
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n"
# The shader cache of the test runs stays in the build tree (set D3D12METAL_CACHE_DIR to choose another).
export D3D12METAL_CACHE_DIR="${D3D12METAL_CACHE_DIR:-$root/build-wine/shader-cache}"
export WINEDEBUG="${WINEDEBUG:--all}"
unset DISPLAY

cleanup() { "$WINE_ROOT/bin/wineserver" -k >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

cd "$godot_dir" || exit 2
"$WINE_ROOT/bin/wine" "$exe" --rendering-driver d3d12 --rendering-method "$method" \
    --path "$root/tests/godot/project" -- --frames="$frames" --out="Z:$shot" >"$log" 2>&1 &
pid=$!
( sleep 240; kill -9 "$pid" 2>/dev/null ) &
watchdog=$!
wait "$pid"
kill "$watchdog" 2>/dev/null
wait "$watchdog" 2>/dev/null

failures=0
fail() { echo "FAIL  godot $method: $1 (log: $log)"; failures=$((failures + 1)); }
grep -a "RENDERER:" "$log" || fail "no RENDERER line (Godot did not start)"
fps=$(grep -a "^FPS:" "$log" | head -1)
[ -n "$fps" ] && echo "$fps" || fail "no FPS line (the run did not finish)"
grep -a "^ERROR\|^USER ERROR" "$log" | sort | uniq -c | sort -rn | head -5
if [ -f "$shot" ]; then
    python3 -I "$root/tools/check_godot_screenshot.py" "$shot" || fail "screenshot check"
else
    fail "no screenshot"
fi
[ "$failures" -eq 0 ] && echo "PASS  godot $method ($shot)"
exit "$failures"
