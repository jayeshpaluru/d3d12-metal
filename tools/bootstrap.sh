#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# One command from a fresh checkout to Alloy.app: checks the machine, installs the Homebrew tools, fetches and builds
# DirectXShaderCompiler (for the HLSL compiler and the DXBC converter) when missing, builds the translation layer and the app.
# Every step is skipped when its result already exists, so it is safe to run again.
#
# Usage: tools/bootstrap.sh [--yes] [--no-app] [--no-native] [--deps DIR]
#   --yes         do not ask before installing Homebrew packages or starting the long DXC build
#   --no-app      stop after the layer (no Swift/SwiftUI build)
#   --no-native   skip the native (arm64) build used by the tests; the layer and app do not need it
#   --deps DIR    where DXC is fetched and built [${DEPS_DIR:-$HOME/code/deps}]
# What it cannot do for you: install Apple's Metal Shader Converter (a download from Apple, see docs/LICENSING.md) and
# Xcode (only its SwiftUI macro plug-in is used, Xcode need not be selected or licensed; it is needed for the app only).
# The DXC build takes 30 to 60 minutes the first time. Wine is downloaded later, by the app or `alloy setup`.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
yes=0 app=1 native=1
DEPS_DIR="${DEPS_DIR:-$HOME/code/deps}"
while [ $# -gt 0 ]; do
    case "$1" in
        --yes|-y) yes=1; shift ;;
        --no-app) app=0; shift ;;
        --no-native) native=0; shift ;;
        --deps) DEPS_DIR="$2"; shift 2 ;;
        -h|--help) sed -n '3,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
export DEPS_DIR
# The commit of DirectXShaderCompiler the layer is developed against.
dxc_ref="${DXC_REF:-75fba61961026d7c763ef41f600b60d7644e237e}"
dxc_src="${DXC_SRC:-$DEPS_DIR/dxc-src}"
dxc_build="${DXC_BUILD_ARM64:-$DEPS_DIR/dxc-build}"

say() { printf '\n==> %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }
ask() { [ "$yes" = 1 ] && return 0; read -r -p "$1 [y/N] " a; [ "$a" = y ] || [ "$a" = Y ]; }

say "Checking the machine"
[ "$(uname -s)" = Darwin ] || die "macOS is required"
[ "$(uname -m)" = arm64 ] || die "an Apple Silicon Mac is required"
major="$(sw_vers -productVersion | cut -d. -f1)"
[ "$major" -ge 15 ] || echo "warning: macOS $major: Rosetta's AVX2 support (needed by most modern games) arrived in macOS 15"
xcode-select -p >/dev/null 2>&1 || die "install the Xcode Command Line Tools first: xcode-select --install"
/usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null || die "install Rosetta 2: softwareupdate --install-rosetta --agree-to-license"
if [ "$app" = 1 ] && [ ! -f /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/usr/lib/swift/host/plugins/libSwiftUIMacros.dylib ]; then
    die "the app needs Xcode installed in /Applications (for SwiftUI's macro plug-in; it need not be selected). Install it from the App Store, or use --no-app"
fi
if [ ! -f /usr/local/lib/libmetalirconverter.dylib ]; then
    echo "note: Apple's Metal Shader Converter is not installed (/usr/local/lib/libmetalirconverter.dylib)."
    echo "      Download it from https://developer.apple.com/metal/shader-converter/ and install the package; the layer cannot be built or run without it."
    die "Metal Shader Converter missing"
fi

say "Homebrew packages"
command -v brew >/dev/null || die "Homebrew is required (https://brew.sh)"
missing=()
for f in meson ninja cmake mingw-w64 directx-headers; do brew list --formula "$f" >/dev/null 2>&1 || missing+=("$f"); done
if [ ${#missing[@]} -gt 0 ]; then
    echo "missing: ${missing[*]}"
    ask "brew install ${missing[*]}?" || die "install them and run again"
    brew install "${missing[@]}"
else
    echo "all present"
fi

say "DirectXShaderCompiler ($dxc_src)"
if [ ! -d "$dxc_src/projects/dxilconv" ]; then
    ask "clone DirectXShaderCompiler into $dxc_src (about 300 MB)?" || die "set DXC_SRC to an existing checkout"
    mkdir -p "$(dirname "$dxc_src")"
    git clone https://github.com/microsoft/DirectXShaderCompiler.git "$dxc_src"
    git -C "$dxc_src" checkout "$dxc_ref"
    git -C "$dxc_src" submodule update --init --recursive
else
    echo "found"
fi
if [ ! -x "$dxc_build/bin/dxc" ]; then
    ask "build dxc (HLSL compiler) in $dxc_build now (30 to 60 minutes)?" || die "build dxc and run again"
    mkdir -p "$dxc_build"
    cmake -G Ninja -S "$dxc_src" -B "$dxc_build" -C "$dxc_src/cmake/caches/PredefinedParams.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DENABLE_SPIRV_CODEGEN=OFF -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    ninja -C "$dxc_build" dxc
else
    echo "dxc found: $dxc_build/bin/dxc"
fi
dxc="$dxc_build/bin/dxc"

say "DXBC converter (libdxilconv)"
if [ -f "$root/build-dxilconv/x86_64/libdxilconv.dylib" ] && { [ "$native" = 0 ] || [ -f "$root/build-dxilconv/arm64/libdxilconv.dylib" ]; }; then
    echo "already built"
else
    if [ "$native" = 1 ]; then "$root/tools/build-dxilconv.sh"; else "$root/tools/build-dxilconv.sh" x86_64; fi
fi

say "Translation layer (Wine build)"
"$root/tools/build-wine.sh" --dxc "$dxc"

if [ "$native" = 1 ]; then
    say "Native build and tests"
    [ -f "$root/build/build.ninja" ] || meson setup "$root/build" "$root" -Ddxc="$dxc"
    meson compile -C "$root/build"
    meson test -C "$root/build" --print-errorlogs || echo "warning: some native tests failed (a busy GPU can cause flaky ones: run them again)"
fi

if [ "$app" = 1 ]; then
    say "Alloy.app"
    "$root/tools/build-app.sh"
fi

say "Done"
echo "Open the app:  open \"$root/build-app/Alloy.app\""
echo "Or the CLI:    $root/build-app/Alloy.app/Contents/Resources/bin/alloy doctor"
