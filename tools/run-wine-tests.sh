#!/bin/bash
# Runs the Win32 tests under Wine (build them first with tools/build-wine.sh):
#   wine_basic.exe                  device, copies, offscreen triangle, fence events
#   p_*.exe                         the portable tests (tests/portable), also run natively
#   hello_samples.exe texture|constbuffers   D3D12HelloTexture / HelloConstBuffers in a window
#   swapchain_test.exe              swap chain on a window: latency object, resize, formats, outputs
#   hello_triangle.exe --selftest   renders in a window, reads the back buffer, checks pixels
#   hello_triangle.exe --frames 300 on screen; the presented frame is checked, and so is a
#                                   screenshot of the window when macOS allows one
# Prints PASS/FAIL per test; exits non-zero if any test failed.
#
# Environment (defaults in brackets):
#   WINE_ROOT   directory with bin/wine and bin/wineserver [the Gcenx Wine Devel app in deps]
#   WINEPREFIX  an initialized prefix [deps/wineprefix]
#   D3D12METAL_LOG=1 additionally traces every bridge call on both sides.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/build-wine/out"
screens="$root/build-wine/screens"
logs="$root/build-wine/logs"
WINE_ROOT="${WINE_ROOT:-/Users/jsp/code/deps/wine/Wine Devel.app/Contents/Resources/wine}"
export WINEPREFIX="${WINEPREFIX:-/Users/jsp/code/deps/wineprefix}"

for path in "$out/d3d12.dll" "$out/dxgi.dll" "$out/x86_64-unix/d3d12metal.so" "$out/wine_basic.exe" \
            "$out/swapchain_test.exe" "$out/hello_triangle.exe" "$out/hello_samples.exe" "$WINE_ROOT/bin/wine"; do
    [ -e "$path" ] || { echo "missing $path (run tools/build-wine.sh first)" >&2; exit 2; }
done
mkdir -p "$screens" "$logs"

# Our DLLs are native overrides loaded from next to the test programs; the unix
# half is found through WINEDLLPATH (<dir>/x86_64-unix/d3d12metal.so).
export WINEDLLPATH="$out"
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n"
# The shader cache of the test runs stays in the build tree (set D3D12METAL_CACHE_DIR to choose another).
export D3D12METAL_CACHE_DIR="${D3D12METAL_CACHE_DIR:-$root/build-wine/shader-cache}"
export WINEDEBUG="${WINEDEBUG:--all}"
unset DISPLAY

wine() { "$WINE_ROOT/bin/wine" "$@"; }
cleanup() { "$WINE_ROOT/bin/wineserver" -k >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

failures=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1: $2"; failures=$((failures + 1)); }

# run_limited <seconds> <log> <command...>: runs with a time limit, returns its exit status.
run_limited() {
    local limit="$1" log="$2"
    shift 2
    "$@" >"$log" 2>&1 &
    local pid=$!
    ( sleep "$limit"; kill -9 "$pid" 2>/dev/null ) &
    local watchdog=$!
    wait "$pid"
    local status=$?
    kill "$watchdog" 2>/dev/null
    wait "$watchdog" 2>/dev/null
    return $status
}

# check_pixel <png> <x> <y> <r> <g> <b> <tolerance>: succeeds when the pixel is within tolerance.
check_pixel() {
    local value
    value="$(python3 -I "$root/tools/png_pixel.py" "$1" "$2" "$3")" || return 1
    IFS=, read -r r g b <<<"$value"
    echo "      pixel ($2,$3) = ($r,$g,$b), expected ~($4,$5,$6)" >&2
    local tolerance="$7" d
    for d in $((r - $4)) $((g - $5)) $((b - $6)); do
        [ "${d#-}" -le "$tolerance" ] || return 1
    done
}

# --- wine_basic -----------------------------------------------------------------------
echo "== wine_basic"
cd "$out"
if run_limited 180 "$logs/wine_basic.log" wine wine_basic.exe && grep -q "^wine_basic: PASS" "$logs/wine_basic.log"; then
    pass "wine_basic"
else
    fail "wine_basic" "see $logs/wine_basic.log"
    tail -20 "$logs/wine_basic.log"
fi
cleanup

# --- portable tests (tests/portable): the same sources as the native suite ------------------
for exe in "$out"/p_*.exe; do
    name="$(basename "$exe" .exe)"
    echo "== $name"
    if run_limited 300 "$logs/$name.log" wine "$name.exe"; then
        pass "$name"
    else
        fail "$name" "see $logs/$name.log"
        tail -20 "$logs/$name.log"
    fi
    cleanup
done

# --- hello_samples: D3D12HelloTexture and D3D12HelloConstBuffers in a window ------------------
for sample in texture constbuffers; do
    echo "== hello_samples $sample"
    if run_limited 180 "$logs/hello_$sample.log" wine hello_samples.exe "$sample" --frames 120 \
       && grep -q "hello_samples $sample: PASS" "$logs/hello_$sample.log"; then
        pass "hello_samples $sample ($(grep -E "^hello_samples $sample:.*(checked|offset)" "$logs/hello_$sample.log" | head -1))"
    else
        fail "hello_samples $sample" "see $logs/hello_$sample.log"
        tail -10 "$logs/hello_$sample.log"
    fi
    cleanup
done

# --- swapchain_test ----------------------------------------------------------------------
echo "== swapchain_test"
if run_limited 180 "$logs/swapchain_test.log" wine swapchain_test.exe && grep -q "^swapchain_test: PASS" "$logs/swapchain_test.log"; then
    pass "swapchain_test"
else
    fail "swapchain_test" "see $logs/swapchain_test.log"
    grep swapchain_test "$logs/swapchain_test.log" | tail -10
fi
cleanup

# --- hello_triangle --selftest -----------------------------------------------------------
echo "== hello_triangle --selftest"
if run_limited 180 "$logs/hello_selftest.log" wine hello_triangle.exe --selftest --frames 120 \
   && grep -q "selftest PASS" "$logs/hello_selftest.log"; then
    pass "hello_triangle selftest ($(grep -E 'hello_triangle: center' "$logs/hello_selftest.log" | head -1))"
else
    fail "hello_triangle selftest" "see $logs/hello_selftest.log"
    grep hello_triangle "$logs/hello_selftest.log" | tail -10
fi
cleanup

# --- hello_triangle on screen ---------------------------------------------------------------
echo "== hello_triangle on screen (300 frames)"
rm -f "$screens/present.png" "$screens/window.png"
D3D12METAL_DUMP_PRESENT="$screens/present.png" D3D12METAL_DUMP_PRESENT_FRAME=60 \
    wine hello_triangle.exe --frames 300 >"$logs/hello_screen.log" 2>&1 &
app=$!

# Wait for the window to be up (the app prints its screen rectangle at frame 30).
for _ in $(seq 1 600); do
    grep -q "WINDOW client_screen_rect" "$logs/hello_screen.log" 2>/dev/null && break
    kill -0 "$app" 2>/dev/null || break
    sleep 0.1
done
grep "WINDOW client_screen_rect" "$logs/hello_screen.log" | head -1

# The real screenshot: needs an awake, unlocked display and Screen Recording permission.
finder="$root/build-wine/find_window"
if [ ! -x "$finder" ] || [ "$root/tools/find_window.swift" -nt "$finder" ]; then
    swiftc -O "$root/tools/find_window.swift" -o "$finder" 2>"$logs/swiftc.log" || finder=""
fi
screenshot_state="unavailable (could not build tools/find_window.swift)"
if [ -n "$finder" ]; then
    window="$("$finder" "D3D12 Hello Triangle" 2>/dev/null)"
    case "$window" in
    "ok "*)
        if screencapture -x -o -l "${window#ok }" "$screens/window.png" 2>/dev/null && [ -s "$screens/window.png" ]; then
            screenshot_state="captured"
        else
            screenshot_state="unavailable (screencapture failed)"
        fi
        ;;
    *) screenshot_state="unavailable (${window:-no answer})" ;;
    esac
fi

wait "$app"
status=$?
fps="$(grep -E 'hello_triangle: [0-9]+ frames' "$logs/hello_screen.log" | head -1)"
echo "      $fps"
if [ "$status" -eq 0 ] && [ -n "$fps" ]; then
    pass "hello_triangle ran 300 frames ($fps)"
else
    fail "hello_triangle 300 frames" "exit status $status, see $logs/hello_screen.log"
fi

# What the layer was given: the present pass rendered into a readable texture.
if [ -s "$screens/present.png" ]; then
    if check_pixel "$screens/present.png" 0.5 0.5 128 64 64 12 \
       && check_pixel "$screens/present.png" 2 2 0 51 102 3; then
        pass "presented frame: triangle in the centre, clear colour in the corner ($screens/present.png)"
    else
        fail "presented frame" "unexpected pixels in $screens/present.png"
    fi
else
    fail "presented frame" "no $screens/present.png (see $logs/hello_screen.log)"
fi

# The real window screenshot, when macOS allowed one.
if [ "$screenshot_state" = "captured" ]; then
    if check_pixel "$screens/window.png" 0.5 0.55 128 64 64 48; then
        pass "window screenshot shows the triangle ($screens/window.png)"
    else
        fail "window screenshot" "unexpected centre pixel in $screens/window.png"
    fi
else
    echo "SKIP  window screenshot: $screenshot_state"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "all Wine tests passed"
else
    echo "$failures Wine test(s) FAILED"
fi
exit $((failures > 0))
