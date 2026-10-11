#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Builds the macOS app: build-app/<App name>.app (ad-hoc signed), with the command line tool, the layer build, the game
# profiles, the licence files and the CPU probe inside. Needs SwiftPM (Command Line Tools are enough) and a layer build
# (tools/build-wine.sh) unless --no-layer.
#
# Usage: tools/build-app.sh [--no-layer] [--debug]
# The app name comes from app/Sources/AlloyCore/AppInfo.swift (AppInfo.name), the single place to rename the product.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
config=release layer=1
while [ $# -gt 0 ]; do
    case "$1" in
        --no-layer) layer=0; shift ;;
        --debug) config=debug; shift ;;
        -h|--help) sed -n '3,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

name="$(sed -n 's/.*static let name = "\(.*\)".*/\1/p' "$root/app/Sources/AlloyCore/AppInfo.swift" | head -1)"
version="$(sed -n 's/.*static let version = "\(.*\)".*/\1/p' "$root/app/Sources/AlloyCore/AppInfo.swift" | head -1)"
bundle_id="$(sed -n 's/.*static let bundleIdentifier = "\(.*\)".*/\1/p' "$root/app/Sources/AlloyCore/AppInfo.swift" | head -1)"
app="$root/build-app/$name.app"
out="$root/build-wine/out"

echo "building the Swift products ($config)..."
(cd "$root/app" && swift build -c "$config" 2>&1 | grep -v "search path" | tail -3)
bin="$(cd "$root/app" && swift build -c "$config" --show-bin-path 2>/dev/null)"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/bin" "$app/Contents/Resources/licenses"
cp "$bin/AlloyApp" "$app/Contents/MacOS/AlloyApp"
cp "$bin/alloy" "$app/Contents/Resources/bin/alloy"

# The profiles, licences and the x86-64 CPU probe (run under Rosetta by `doctor`).
cp -R "$root/profiles" "$app/Contents/Resources/profiles"
cp "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$app/Contents/Resources/licenses/"
cp "$root"/third_party/licenses/* "$app/Contents/Resources/licenses/"
clang -arch x86_64 -O1 -o "$app/Contents/Resources/cpuprobe" "$root/app/Resources/cpuprobe.c"
mkdir -p "$root/build-app" && cp "$app/Contents/Resources/cpuprobe" "$root/build-app/cpuprobe"

# The layer: the DLLs, the unix half, the DXBC converter and the Steam web helper shim (no test programs).
if [ "$layer" = 1 ]; then
    [ -f "$out/d3d12.dll" ] || { echo "no layer build in $out: run tools/build-wine.sh first (or --no-layer)" >&2; exit 1; }
    mkdir -p "$app/Contents/Resources/layer/x86_64-unix"
    cp "$out/d3d12.dll" "$out/dxgi.dll" "$out/layer-version.txt" "$app/Contents/Resources/layer/"
    [ -f "$out/steamwebhelper-shim.exe" ] && cp "$out/steamwebhelper-shim.exe" "$app/Contents/Resources/layer/"
    cp "$out/x86_64-unix/"* "$app/Contents/Resources/layer/x86_64-unix/"
fi

# Icon.
iconset="$(mktemp -d)/AppIcon.iconset"
swift "$root/tools/make-icon.swift" "$iconset" 2>/dev/null && iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns" || echo "note: no icon"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>$name</string>
    <key>CFBundleDisplayName</key><string>$name</string>
    <key>CFBundleIdentifier</key><string>$bundle_id</string>
    <key>CFBundleExecutable</key><string>AlloyApp</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSHumanReadableCopyright</key><string>LGPL-2.1-or-later</string>
</dict>
</plist>
EOF

# Ad-hoc signature (no developer account: you build and run this yourself; it is not notarized or distributed).
codesign --force --deep -s - "$app"
codesign --verify --deep --strict "$app" && echo "signed (ad hoc)"
echo "built $app"
