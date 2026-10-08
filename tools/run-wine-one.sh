#!/bin/bash
# Runs one Win32 program of the Wine build (build-wine/out) in the test prefix, e.g. tools/run-wine-one.sh p_descbench.exe.
# Same environment as tools/run-wine-tests.sh; the Steam prefix is never touched.
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/build-wine/out"
WINE_ROOT="${WINE_ROOT:-/Users/jsp/code/deps/wine/Wine Devel.app/Contents/Resources/wine}"
export WINEPREFIX="${WINEPREFIX:-/Users/jsp/code/deps/wineprefix}"
export WINEDLLPATH="$out" WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n" WINEDEBUG="${WINEDEBUG:--all}"
export D3D12METAL_CACHE_DIR="${D3D12METAL_CACHE_DIR:-$root/build-wine/shader-cache}"
cd "$out" && exec "$WINE_ROOT/bin/wine" "$@"
