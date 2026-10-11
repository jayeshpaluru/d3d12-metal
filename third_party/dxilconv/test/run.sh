#!/bin/bash
# Builds and runs window_test against build-dxilconv/arm64/libdxilconv.dylib (run tools/build-dxilconv.sh arm64 first).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
dxc="${DXC:-/Users/jsp/code/deps/dxc-build/bin/dxc}"
work="$root/build-dxilconv/window-test"
mkdir -p "$work"
clang++ -std=c++17 -O1 -ObjC++ -fobjc-arc -I/usr/local/include "$here/window_test.mm" -o "$work/window_test" \
    -framework Metal -framework Foundation \
    -L/usr/local/lib -lmetalirconverter -L"$root/build-dxilconv/arm64" -ldxilconv \
    -Wl,-rpath,/usr/local/lib -Wl,-rpath,"$root/build-dxilconv/arm64"
"$work/window_test" "$dxc" "$here/shaders" "$work/out"
