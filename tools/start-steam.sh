#!/bin/bash
# Launch the Windows Steam client in its own Wine prefix (kept separate from the
# test prefix, whose wineserver the test scripts kill).
#   WINE_ROOT          directory with bin/wine [the Gcenx Wine Devel app in deps]
#   STEAM_WINEPREFIX   prefix with Steam installed [deps/wineprefix-steam]
set -euo pipefail
WINE_ROOT="${WINE_ROOT:-/Users/jsp/code/deps/wine/Wine Devel.app/Contents/Resources/wine}"
export WINEPREFIX="${STEAM_WINEPREFIX:-/Users/jsp/code/deps/wineprefix-steam}"
export WINEDEBUG="${WINEDEBUG:--all}"
exec "$WINE_ROOT/bin/wine" 'C:\Program Files (x86)\Steam\steam.exe' -no-cef-sandbox -cef-disable-gpu -noverifyfiles "$@"
