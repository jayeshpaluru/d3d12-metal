#!/bin/bash
# Undoes tools/install-game.sh: restores the files it backed up (*.d3d12metal-orig), removes the ones it added and
# the per-app DLL overrides from the Wine prefix's registry. Safe to run again.
#
# Usage: tools/uninstall-game.sh [--dry-run]
# Environment: GAME_DIR, GAME_EXE, STEAM_WINEPREFIX, WINE_ROOT (see tools/game-common.sh)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/game-common.sh"

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
need_wine

if find_game_dir; then
    manifest="$game_dir/$manifest_name"
    echo "game directory: $game_dir"
    if [ -f "$manifest" ]; then
        while IFS='|' read -r rel backup; do
            [ -n "$rel" ] || continue
            if [ "$rel" = "x86_64-unix/" ]; then
                continue   # the directory goes last, once its file is gone
            fi
            if [ "$backup" != "-" ] && [ -e "$game_dir/$backup" ]; then
                echo "restore:      $backup -> $rel"
                run mv -f "$game_dir/$backup" "$game_dir/$rel"
            elif [ -e "$game_dir/$rel" ]; then
                echo "remove:       $rel"
                run rm -f "$game_dir/$rel"
            fi
        done < "$manifest"
        if grep -q '^x86_64-unix/|' "$manifest" && [ -d "$game_dir/x86_64-unix" ]; then
            run rmdir "$game_dir/x86_64-unix" 2>/dev/null || echo "kept:         x86_64-unix/ (not empty)"
        fi
        run rm -f "$manifest"
    else
        echo "nothing installed in the game directory (no $manifest_name)"
    fi
else
    echo "(no game directory: only the registry is cleaned)"
fi

# Remove the overrides this layer set; delete the key when nothing else is left in it.
echo "registry:     removing ${override_dlls[*]} from $override_key"
for dll in "${override_dlls[@]}"; do
    if wine_cmd reg query "$override_key" /v "$dll" >/dev/null 2>&1; then
        run wine_cmd reg delete "$override_key" /v "$dll" /f >/dev/null
    fi
done
if [ "$dry_run" = 0 ] && wine_cmd reg query "$override_key" >/dev/null 2>&1 \
   && ! wine_cmd reg query "$override_key" 2>/dev/null | grep -q REG_; then
    wine_cmd reg delete "$override_key" /f >/dev/null
fi
echo "done."
