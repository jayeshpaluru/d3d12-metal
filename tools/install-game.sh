#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Installs the layer next to a game: d3d12.dll and dxgi.dll beside the exe, d3d12metal.so in x86_64-unix/, a default
# d3d12metal.conf (log file, API trace), and per-app DLL overrides in the Wine prefix's registry so they apply when
# Steam launches the game (the environment of a shell does not reach it):
#   HKCU\Software\Wine\AppDefaults\<exe>\DllOverrides   d3d12, d3d12core, dxgi = native,builtin
# Files it would overwrite are renamed *.d3d12metal-orig first. The game's own D3D12\ folder (D3D12Core.dll) is left
# alone: the layer ignores the Agility SDK path. Safe to run again; tools/uninstall-game.sh undoes it.
# The registry is changed with `wine reg add`, which talks to the prefix's running wineserver (Steam keeps running).
#
# Usage: tools/install-game.sh [--dry-run]       (build first: tools/build-wine.sh)
# Environment: SPOOF_GPU=amd (see below), GAME (sm1|sm2, default sm1), GAME_DIR, GAME_EXE, STEAM_WINEPREFIX, WINE_ROOT (see tools/game-common.sh)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/game-common.sh"

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

for f in d3d12.dll dxgi.dll x86_64-unix/d3d12metal.so; do
    [ -f "$game_out/$f" ] || { echo "missing $game_out/$f (run tools/build-wine.sh first)" >&2; exit 2; }
done
need_wine
find_game_dir || exit 1
manifest="$game_dir/$manifest_name"
echo "game directory: $game_dir"

# The manifest lists what this script put in the game directory: "<path relative to it>|<backup name or ->".
manifest_tmp="$(mktemp)"
trap 'rm -f "$manifest_tmp"' EXIT
[ -f "$manifest" ] && cp "$manifest" "$manifest_tmp"
recorded() { grep -q "^$1|" "$manifest_tmp"; }
record() {
    grep -v "^$1|" "$manifest_tmp" > "$manifest_tmp.new" || true
    echo "$1|$2" >> "$manifest_tmp.new"
    mv "$manifest_tmp.new" "$manifest_tmp"
}

# install_file <source> <path relative to the game directory>
install_file() {
    local src="$1" rel="$2" dest="$game_dir/$2" backup="-"
    if [ -e "$dest" ]; then
        if cmp -s "$src" "$dest"; then
            echo "up to date:   $rel"
            recorded "$rel" || record "$rel" "-"
            return
        fi
        if recorded "$rel"; then
            backup="$(grep "^$rel|" "$manifest_tmp" | head -1 | cut -d'|' -f2)"   # replacing an older build of ours
        else
            backup="$rel.d3d12metal-orig"
            local n=1
            while [ -e "$game_dir/$backup" ]; do backup="$rel.d3d12metal-orig.$n"; n=$((n + 1)); done
            echo "backup:       $rel -> $backup"
            run mv "$dest" "$game_dir/$backup"
        fi
    fi
    echo "install:      $rel"
    run mkdir -p "$(dirname "$dest")"
    run cp "$src" "$dest"
    record "$rel" "$backup"
}

if [ ! -d "$game_dir/x86_64-unix" ]; then
    record "x86_64-unix/" "-"
fi
install_file "$game_out/d3d12.dll" d3d12.dll
install_file "$game_out/dxgi.dll" dxgi.dll
install_file "$game_out/x86_64-unix/d3d12metal.so" x86_64-unix/d3d12metal.so
if [ -f "$game_out/x86_64-unix/libdxilconv.dylib" ]; then
    install_file "$game_out/x86_64-unix/libdxilconv.dylib" x86_64-unix/libdxilconv.dylib
else
    echo "note: no libdxilconv.dylib (DXBC shaders will be refused): tools/build-dxilconv.sh, then tools/build-wine.sh" >&2
fi

# The configuration is written once; edit it freely (a second install keeps your changes).
conf="$game_dir/d3d12metal.conf"
if [ -e "$conf" ]; then
    echo "kept:         d3d12metal.conf (already there)"
else
    echo "install:      d3d12metal.conf (log: $game_logs/d3d12metal.log)"
    if [ "$dry_run" = 0 ]; then
        mkdir -p "$game_logs"
        cat > "$conf" <<EOF
# d3d12-metal configuration, read once when the game loads d3d12.dll. Environment variables D3D12METAL_<KEY>
# override these. Keys: log (every bridge call, very verbose), log_file, trace, stats, dump_failed, cache_dir,
# cache (0 = off), cache_max_mb, dump_present, dump_present_frame, no_barriers, async_submit (0 = off).
log_file=$game_logs/d3d12metal.log
trace=1
stats=1
dump_failed=$game_logs/failed-shaders
#log=1
#cache_dir=$game_root/build-wine/shader-cache-game
EOF
    fi
    record "d3d12metal.conf" "-"
fi

# SPOOF_GPU=nvidia|amd|apple (default: the profile's, GAME_SPOOF_GPU): the identity the adapter presents, for games that
# pick code paths or whitelist GPUs by vendor. Each install rewrites the identity lines of the conf (adapter_name,
# vendor_id, device_id, driver_version); apple leaves them out (the adapter Wine registered). The layer itself registers
# the driver keys a game looks up for a spoofed identity (volatile, when the game makes its first adapter: Wine prunes
# display devices it did not detect, so keys written here would not last).
# The profile's own conf lines (GAME_CONF: feature_level=12_1 for sm2, whose startup check wants rasterizer ordered
# views) are rewritten the same way.
spoof="${SPOOF_GPU:-$GAME_SPOOF_GPU}"
case "$spoof" in
    nvidia) identity=("NVIDIA GeForce RTX 3070" 10de 2484 32.0.15.6094) ;;
    amd) identity=("AMD Radeon RX 6800" 1002 73bf 31.0.24033.1003) ;;
    apple) identity=() ;;
    *) echo "unknown SPOOF_GPU=$spoof (nvidia, amd or apple)" >&2; exit 2 ;;
esac
if [ "$dry_run" = 0 ] && [ -f "$conf" ]; then
    grep -v -E '^(adapter_name|vendor_id|device_id|driver_version|feature_level)=' "$conf" > "$conf.new" || true
    [ -n "$GAME_CONF" ] && printf '%s\n' $GAME_CONF >> "$conf.new"
    mv "$conf.new" "$conf"
fi
[ -n "$GAME_CONF" ] && echo "conf:         $GAME_CONF"
if [ ${#identity[@]} -gt 0 ]; then
    echo "spoof:        adapter ${identity[0]} (${identity[1]}:${identity[2]}), driver ${identity[3]}"
    if [ "$dry_run" = 0 ]; then
        printf 'adapter_name=%s\nvendor_id=%s\ndevice_id=%s\ndriver_version=%s\n' "${identity[@]}" >> "$conf"
    fi
else
    echo "spoof:        none (the adapter Wine registered)"
fi

# Game-specific quirks come from profiles/*.json (the d3d12metal CLI writes them); this dev script knows Spider-Man 2's.
if [ "${GAME:-}" = sm2 ] && [ "$dry_run" = 0 ] && ! grep -q '^quirk_force_compute_barrier=' "$conf"; then
    echo 'quirk_force_compute_barrier=0x324071d329f05ccc' >> "$conf"
fi

if [ "$dry_run" = 0 ]; then
    cp "$manifest_tmp" "$manifest"
else
    echo "dry-run: would write $manifest"
fi

# DLL overrides for this exe only.
echo "registry:     $override_key = native,builtin for ${override_dlls[*]}"
for dll in "${override_dlls[@]}"; do
    run wine_cmd reg add "$override_key" /v "$dll" /t REG_SZ /d native,builtin /f >/dev/null
done
if [ "$dry_run" = 0 ]; then
    wine_cmd reg query "$override_key" | tr -d '\r' | grep REG_SZ | sed 's/^ */  /'
fi
echo "done."
