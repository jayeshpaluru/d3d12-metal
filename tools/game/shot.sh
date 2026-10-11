#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Asks the running game (launched by tools/game/menu-scene.sh <tag>, which sets D3D12METAL_DUMP_PRESENT) for a dump of
# one of its next presents and prints its path: works without screen-capture permission and with the session locked.
# usage: tools/game/shot.sh <tag> [timeout seconds]
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
tag="${1:?usage: shot.sh <tag>}"; dumps="$game_root/build-wine/dumps"
before=$(ls -t "$dumps/$tag"-*.png 2>/dev/null | head -1)
touch "$dumps/$tag.png.go"
for _ in $(seq $((${2:-20} * 5))); do
    newest=$(ls -t "$dumps/$tag"-*.png 2>/dev/null | head -1)
    if [ -n "$newest" ] && [ "$newest" != "$before" ] && [ ! -e "$dumps/$tag.png.go" ]; then
        sleep 0.5   # the PNG is written by a completion handler
        echo "$newest"
        exit 0
    fi
    sleep 0.2
done
rm -f "$dumps/$tag.png.go"
echo "no dump within ${2:-20}s" >&2
exit 1
