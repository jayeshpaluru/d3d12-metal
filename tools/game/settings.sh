#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# The game's graphics settings: Nixxes ports keep them in the registry (HKCU\Software\Insomniac Games\<game>\Graphics),
# apart from the saves, which this never touches. Run while the game is stopped (it writes the key when it exits).
#   GAME=sm1|sm2 tools/game/settings.sh show                  prints the values
#   GAME=sm1|sm2 tools/game/settings.sh backup [file]         exports the key (default build-wine/settings-backup/<game>-graphics.reg;
#                                                              an existing backup is kept: it holds the user's own settings)
#   GAME=sm1|sm2 tools/game/settings.sh restore [file]        deletes the key and imports the backup
#   GAME=sm1|sm2 tools/game/settings.sh set Name=dword ...    e.g. VSync=0 DynamicResolutionTargetFPS=0 (values in decimal)
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
case "$GAME" in
    sm1) title="Marvel's Spider-Man Remastered" ;;
    sm2) title="Marvel's Spider-Man 2" ;;
    *) echo "unknown GAME=$GAME" >&2; exit 2 ;;
esac
key="HKCU\\Software\\Insomniac Games\\$title\\Graphics"
backup_dir="$game_root/build-wine/settings-backup"
regfile() { echo "${1:-$backup_dir/$GAME-graphics.reg}"; }
winpath() { echo "Z:${1//\//\\}"; }
reg() { WINEDEBUG=-all wine_steam reg "$@"; }
action="${1:?usage: settings.sh show|backup|restore|set Name=value...}"; shift
case "$action" in
    show) reg query "$key" ;;
    backup)
        file="$(regfile "$1")"; mkdir -p "$(dirname "$file")"
        if [ -e "$file" ]; then echo "kept existing backup $file"; exit 0; fi
        reg export "$key" "$(winpath "$file")" /y > /dev/null && echo "backed up to $file" ;;
    restore)
        file="$(regfile "$1")"
        [ -s "$file" ] || { echo "no backup at $file" >&2; exit 1; }
        reg delete "$key" /f > /dev/null 2>&1
        reg import "$(winpath "$file")" && echo "restored from $file" ;;
    set)
        pgrep -f "$GAME_EXE" > /dev/null && { echo "$GAME_EXE is running; it would overwrite the settings when it exits" >&2; exit 2; }
        [ -e "$(regfile)" ] || { echo "back up first: settings.sh backup" >&2; exit 2; }
        for kv in "$@"; do
            reg add "$key" /v "${kv%%=*}" /t REG_DWORD /d "${kv#*=}" /f > /dev/null && echo "set ${kv%%=*}=${kv#*=}"
        done ;;
    *) echo "unknown action $action" >&2; exit 2 ;;
esac
