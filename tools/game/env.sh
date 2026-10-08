#!/bin/bash
# Shared by the tools/game scripts (sourced). Paths are overridable through the environment:
#   GAME (sm1|sm2 profile), WINE_ROOT, STEAM_WINEPREFIX (see tools/game-common.sh), GAME_FOLDER (the game's install folder)
source "$(dirname "${BASH_SOURCE[0]}")/../game-common.sh"
STEAM_WINEPREFIX="${STEAM_WINEPREFIX:-/Users/jsp/code/deps/wineprefix-steam}"
default_game_folder="$STEAM_WINEPREFIX/drive_c/Program Files (x86)/Steam/steamapps/common/$GAME_INSTALL_DIR"
GAME_FOLDER="${GAME_FOLDER:-$default_game_folder}"

# wine_steam <args...>: wine in the Steam prefix.
wine_steam() { WINEPREFIX="$STEAM_WINEPREFIX" "$WINE_ROOT/bin/wine" "$@"; }

# game_winedbg_pid: pid of the running game as winedbg lists it (hex, no 0x).
game_winedbg_pid() {
    WINEDEBUG=-all wine_steam winedbg --command "info proc" 2>&1 | grep "'$GAME_EXE'" | awk '{print $1}'
}
