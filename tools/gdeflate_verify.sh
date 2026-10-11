#!/bin/bash
# Builds tools/gdeflate_verify.cpp against the reference GDeflate codec in third_party and runs it on dumped calls.
# usage: tools/gdeflate_verify.sh <dir with call-N.bin>     (dumps: D3D12METAL_GDEFLATE_DUMP=<dir> while the game runs)
set -e
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
t="$root/third_party/libdeflate-gdeflate"
work="$root/build-wine/gdeflate_verify.d"
exe="$work/gdeflate_verify"
if [ ! -x "$exe" ]; then
    mkdir -p "$work"
    for c in deflate_compress deflate_decompress gdeflate_compress gdeflate_decompress utils; do
        clang -O2 -c "$t/lib/$c.c" -I"$t" -I"$t/lib" -I"$t/common" -o "$work/$c.o"
    done
    clang++ -O2 -std=c++17 "$root/tools/gdeflate_verify.cpp" "$work"/*.o -I"$t" -o "$exe"
fi
"$exe" "$1"/call-*.bin
