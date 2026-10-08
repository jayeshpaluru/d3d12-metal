#!/bin/bash
# Sends keys/clicks to the running game from inside Wine (tools/game/keys.c; no macOS Accessibility permission needed).
# usage: tools/game/keys.sh [-w title] [-m sendinput|post] <step>...   e.g. tools/game/keys.sh down enter wait:2000 list
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
exe="$game_root/build-wine/out/keys.exe"
[ -f "$exe" ] || x86_64-w64-mingw32-gcc -O2 -o "$exe" "$game_root/tools/game/keys.c" || exit 1
WINEDEBUG=-all wine_steam "$exe" "$@"
