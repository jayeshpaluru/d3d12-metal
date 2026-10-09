#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Shared by install-game.sh, uninstall-game.sh and run-game.sh (sourced).
#
# Environment (defaults in brackets):
#   WINE_ROOT          directory with bin/wine [the Gcenx Wine Devel app in deps]
#   STEAM_WINEPREFIX   the prefix Steam and the game live in [deps/wineprefix-steam]
#   GAME_WINEPREFIX    act on this prefix instead (stand-in games in the test prefix)
#   GAME              game profile: sm1 (Marvel's Spider-Man Remastered) or sm2 (Marvel's Spider-Man 2) [sm1]
#   GAME_DIR           the folder holding the game's exe [steamapps/common/<install dir of the profile> in the Steam prefix]
#   GAME_EXE           the exe's name [from the profile: Spider-Man.exe, Spider-Man2.exe]
#   GAME_APPID         Steam app id [from the profile: 1817070, 2651280]
#   GAME_ARGS          default game arguments [from the profile: -nolauncher]
#   GAME_OUT           the build to install [build-wine/out] (install-game.sh; an older build for comparisons)

game_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINE_ROOT="${WINE_ROOT:-${DEPS_DIR:-$HOME/code/deps}/wine/Wine Devel.app/Contents/Resources/wine}"

# Game profiles. Each defines the exe, Steam app id, install folder name (under steamapps/common), default game
# arguments and the window title to capture. GAME_EXE, GAME_APPID, GAME_INSTALL_DIR and GAME_ARGS from the
# environment override the profile (stand-in games).
GAME="${GAME:-sm1}"
case "$GAME" in
    sm1) profile_exe="Spider-Man.exe";  profile_appid=1817070; profile_dir="Marvel's Spider-Man Remastered"; profile_args="-nolauncher"; profile_window="Spider" ;;
    sm2) profile_exe="Spider-Man2.exe"; profile_appid=2651280; profile_dir="Marvel's Spider-Man 2";         profile_args="-nolauncher"; profile_window="Spider" ;;
    *) echo "unknown GAME=$GAME (sm1 or sm2)" >&2; exit 2 ;;
esac
GAME_EXE="${GAME_EXE:-$profile_exe}"
GAME_APPID="${GAME_APPID:-$profile_appid}"
GAME_INSTALL_DIR="${GAME_INSTALL_DIR:-$profile_dir}"
GAME_ARGS="${GAME_ARGS-$profile_args}"
GAME_WINDOW="${GAME_WINDOW:-$profile_window}"
# Screen-state references of the classifier (tools/game/state.sh), made locally per game.
game_ref="$game_root/build-wine/ref/$GAME"
game_out="${GAME_OUT:-$game_root/build-wine/out}"
game_logs="$game_root/build-wine/game-logs"
game_screens="$game_root/build-wine/game-screens"
# Per-app overrides: HKCU\Software\Wine\AppDefaults\<exe>\DllOverrides
override_key="HKCU\\Software\\Wine\\AppDefaults\\$GAME_EXE\\DllOverrides"
override_dlls=(d3d12 d3d12core dxgi)
manifest_name=".d3d12metal-install"

dry_run=0
# run <command...>: runs it, or only prints it under --dry-run.
run() {
    if [ "$dry_run" = 1 ]; then
        { printf 'dry-run:'; printf ' %q' "$@"; printf '\n'; } >&2
    else
        "$@"
    fi
}

# The prefix the scripts act on: GAME_WINEPREFIX if set (run-game.sh --direct sets it), else the Steam prefix. The
# ambient WINEPREFIX is ignored on purpose, so a shell left pointing at the test prefix cannot redirect a Steam install.
game_prefix() {
    echo "${GAME_WINEPREFIX:-${STEAM_WINEPREFIX:-${DEPS_DIR:-$HOME/code/deps}/wineprefix-steam}}"
}

# Sets game_dir: GAME_DIR if given, else the first steamapps/common/*/<exe> of the prefix's Steam. Returns 1 if none.
find_game_dir() {
    if [ -n "${GAME_DIR:-}" ]; then
        [ -d "$GAME_DIR" ] || { echo "GAME_DIR $GAME_DIR is not a directory" >&2; return 1; }
        game_dir="${GAME_DIR%/}"
        return 0
    fi
    local common exe
    exe="$(game_prefix)/drive_c/Program Files (x86)/Steam/steamapps/common/$GAME_INSTALL_DIR/$GAME_EXE"
    if [ -f "$exe" ]; then
        game_dir="$(dirname "$exe")"
        return 0
    fi
    common="$(game_prefix)/drive_c/Program Files (x86)/Steam/steamapps/common"
    for exe in "$common"/*/"$GAME_EXE"; do
        if [ -f "$exe" ]; then
            game_dir="$(dirname "$exe")"
            return 0
        fi
    done
    echo "$GAME_EXE not found in $common/$GAME_INSTALL_DIR (GAME=$GAME; still downloading? set GAME_DIR=<folder with $GAME_EXE>)" >&2
    return 1
}

# wine_cmd <args...>: runs wine in the game prefix with no debug output.
wine_cmd() {
    WINEPREFIX="$(game_prefix)" WINEDEBUG="${WINEDEBUG:--all}" "$WINE_ROOT/bin/wine" "$@"
}

need_wine() {
    [ -x "$WINE_ROOT/bin/wine" ] || { echo "no wine at $WINE_ROOT/bin/wine (set WINE_ROOT)" >&2; exit 2; }
    [ -d "$(game_prefix)" ] || { echo "no Wine prefix at $(game_prefix)" >&2; exit 2; }
}
