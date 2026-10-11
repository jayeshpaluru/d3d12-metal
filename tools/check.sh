#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Everything that should pass before a change is merged, in one command:
#   1. no personal data or hardcoded home paths in tracked files, SPDX headers present
#   2. the profiles are valid JSON
#   3. native build + `meson test` (+ the Metal API validation setup with --validation)
#   4. the Swift package: build (CLI, app, Core) and `swift test`
#   5. the app bundle (--app)
# Usage: tools/check.sh [--validation] [--app] [--no-native] [--no-swift]
# Needs: meson, ninja, directx-headers, DXC (D3D12METAL_DXC or on PATH; see README), SwiftPM.
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
validation=0 app=0 native=1 swiftpm=1
for a in "$@"; do
    case "$a" in
        --validation) validation=1 ;;
        --app) app=1 ;;
        --no-native) native=0 ;;
        --no-swift) swiftpm=0 ;;
        -h|--help) sed -n '3,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done
cd "$root"
fail=0
step() { echo; echo "== $*"; }
bad() { echo "FAIL: $*" >&2; fail=1; }

step "personal data and hardcoded paths"
files="$(git ls-files | grep -v '^third_party/' | grep -v '^docs/images/')"
# shellcheck disable=SC2086
if grep -InE "/Users/[A-Za-z0-9._-]+|[A-Za-z0-9._%+-]+@(gmail|icloud|outlook|hotmail)\.|7656119[0-9]{10}" $files | grep -v "/Users/<user>" | grep -v "/Users/alice" | grep -v "tools/check.sh" | grep -v "app/Tests/"; then
    bad "personal paths or ids found (above)"
else
    echo "none"
fi
missing="$(for f in $(git ls-files 'src/*' 'tests/*' 'tools/*' 'app/Sources/*' 'app/Tests/*' | grep -E '\.(cpp|h|mm|c|swift|sh|py)$'); do grep -q SPDX-License-Identifier "$f" || echo "$f"; done)"
[ -z "$missing" ] || { echo "$missing"; bad "files without an SPDX header (above)"; }

step "profiles"
for p in profiles/*.json; do python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$p" || bad "$p is not valid JSON"; done
echo "ok"

if [ "$native" = 1 ]; then
    step "native build and tests"
    dxc="${D3D12METAL_DXC:-}"
    if [ ! -f build/build.ninja ]; then
        if [ -n "$dxc" ]; then meson setup build -Ddxc="$dxc" || bad "meson setup"; else meson setup build || bad "meson setup"; fi
    fi
    meson compile -C build || bad "meson compile"
    meson test -C build --print-errorlogs || bad "meson test"
    [ "$validation" = 1 ] && { meson test -C build --setup validation --print-errorlogs || bad "meson test (validation)"; }
fi

if [ "$swiftpm" = 1 ]; then
    step "Swift package (Core, CLI, app)"
    (cd app && swift build 2>&1 | grep -v "search path" | tail -3) || bad "swift build"
    # The macro plug-in of Swift Testing is sometimes not found on a busy machine (a toolchain flake): retry.
    ok=0
    for attempt in 1 2 3; do
        out="$(cd app && swift test 2>&1 | grep -v "search path")"
        if echo "$out" | grep -q "Test run with .* passed"; then ok=1; echo "$out" | grep "Test run with"; break; fi
        echo "$out" | grep -q "plugin for module 'TestingMacros' not found" || break
        echo "(attempt $attempt: toolchain plug-in not found, retrying)"
    done
    [ "$ok" = 1 ] || { echo "$out" | grep -E "error|✘" | head; bad "swift test"; }
fi

if [ "$app" = 1 ]; then
    step "app bundle"
    tools/build-app.sh || bad "build-app.sh"
fi

echo
if [ "$fail" = 0 ]; then echo "all checks passed"; else echo "CHECKS FAILED"; exit 1; fi
