#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Builds the Wine flavour of d3d12-metal into build-wine/out:
#   d3d12.dll, dxgi.dll              PE front-end (MinGW cross build)
#   x86_64-unix/d3d12metal.so        Metal backend + unix-call table (x86-64 macOS)
#   x86_64-unix/libdxilconv.dylib    DXBC -> DXIL converter (when tools/build-dxilconv.sh has run)
#   wine_basic.exe, hello_triangle.exe   Win32 test programs
#
# Usage: tools/build-wine.sh [--dxc <path to dxc>]    (default: $DXC, dxc on PATH)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dxc="${DXC:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --dxc) dxc="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
dxc_option=()
[ -n "$dxc" ] && dxc_option=("-Ddxc=$dxc")

build="$root/build-wine"
out="$build/out"

setup_and_compile() {
    local dir="$1" cross_file="$2"
    shift 2
    if [ ! -f "$build/$dir/build.ninja" ]; then
        meson setup "$build/$dir" "$root" --cross-file "$root/cross/$cross_file" "$@"
    fi
    meson compile -C "$build/$dir"
}

setup_and_compile mingw mingw-x86_64.txt ${dxc_option[@]+"${dxc_option[@]}"}
setup_and_compile unix macos-x86_64.txt

rm -rf "$out"
mkdir -p "$out/x86_64-unix"
cp "$build/mingw/src/pe/d3d12.dll" "$build/mingw/src/pe/dxgi.dll" "$out/"
cp "$build/mingw/tests/wine/"*.exe "$out/"
cp "$build/unix/d3d12metal.so" "$out/x86_64-unix/"
# DXBC (Shader Model 4/5) shaders need the converter next to the backend (tools/build-dxilconv.sh builds it).
if [ -f "$root/build-dxilconv/x86_64/libdxilconv.dylib" ]; then
    cp "$root/build-dxilconv/x86_64/libdxilconv.dylib" "$out/x86_64-unix/"
else
    echo "note: build-dxilconv/x86_64/libdxilconv.dylib is missing: DXBC shaders will be refused (tools/build-dxilconv.sh)" >&2
fi
echo "built $out:"
ls -R "$out"
