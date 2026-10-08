#!/bin/bash
# Regenerates tests/shaders/dxbc_{vs,ps,cs}.h: tests/shaders/dxbc.hlsl compiled to Shader Model 5 DXBC by the
# d3dcompiler_47 of Wine (there is no fxc on macOS). The headers are committed; run this only to change the shaders.
# Needs tools/build-wine.sh to have run (it builds build-wine/out/dxbc_compile.exe).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/game-common.sh"
tool="$game_out/dxbc_compile.exe"
[ -f "$tool" ] || { echo "$tool missing: run tools/build-wine.sh first" >&2; exit 2; }
need_wine
shaders="$game_root/tests/shaders"
export WINEPREFIX="${GAME_WINEPREFIX:-/Users/jsp/code/deps/wineprefix}" WINEDEBUG=-all
for stage in "VSMain vs_5_0 vs" "PSMain ps_5_0 ps" "CSMain cs_5_0 cs"; do
    set -- $stage
    "$WINE_ROOT/bin/wine" "$tool" "Z:$shaders/dxbc.hlsl" "$1" "$2" "Z:$shaders/dxbc_$3.h" "g_dxbc_$3"
done
