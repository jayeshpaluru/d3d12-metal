#!/bin/bash
# Drives a freshly launched game from the profile menu into the open world (tools/game/keys.sh, no macOS permissions needed).
# It looks at the screen (tools/game/state.sh) before every key: picks the profile row, confirms CONTINUE, waits while loading and
# returns once the open world has been seen twice in a row. Never confirms anything it does not recognise: a screen that is not the
# main menu after a profile was chosen (New Game's difficulty menu) gets Escape. Exits 1 on timeout.
# usage: GAME_LOG=<layer log> tools/game/to-gameplay.sh [profile-row 1|2] [load-seconds, the time allowed after CONTINUE]
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
row="${1:-2}"; load="${2:-120}"
tools="$game_root/tools/game"
# The menu is up once the layer has printed a stats line (launch with D3D12METAL_STATS=1 D3D12METAL_LOG_FILE=<file>, pass the file in GAME_LOG).
log="${GAME_LOG:?set GAME_LOG to the layer log file the game was launched with}"
# (the splash screens print stats lines too; the profile menu is the first with 60+ render passes per frame)
for _ in $(seq 180); do
    grep "stats (per frame" "$log" 2>/dev/null | tail -1 | grep -q "render passes [6-9][0-9]\." && break; sleep 2
done
sleep 3
deadline=$((SECONDS + 120 + load)); continued=0; chose=0; seen=0; s=
while [ $SECONDS -lt $deadline ]; do
    s="$("$tools/state.sh")"
    echo "state: $s"
    case "$s" in
        gameplay) seen=$((seen + 1)); [ $seen -ge 2 ] && break; continue ;;
        profile1|profile2)
            seen=0; [ $continued = 1 ] && continue
            have="${s#profile}"
            if [ "$have" -lt "$row" ]; then "$tools/keys.sh" down wait:800 >/dev/null
            elif [ "$have" -gt "$row" ]; then "$tools/keys.sh" up wait:800 >/dev/null
            else "$tools/keys.sh" enter wait:3000 >/dev/null; chose=1; fi ;;
        main-continue) seen=0; "$tools/keys.sh" enter >/dev/null; continued=1; chose=0; deadline=$((SECONDS + load)) ;;
        *) seen=0
            # after choosing a profile the main menu comes within seconds; anything else there is a menu we must not confirm
            if [ $chose = 1 ] && [ $continued = 0 ]; then
                sleep 3
                if [ "$("$tools/state.sh")" = unknown ]; then echo "state: unknown after the profile, Escape"; "$tools/keys.sh" esc wait:1000 >/dev/null; fi
                chose=0
            fi ;;
    esac
    sleep 2
done
[ "$s" = gameplay ] || { echo "to-gameplay: no gameplay within the time allowed (last state: $s)" >&2; exit 1; }
# Gameplay only reads the keyboard once the window has been clicked (menus take keys without it). The click also ends the
# crouching start of the save (Spider-Man walks off), so it is opt-in: GAMEPLAY_CLICK=1 (measurements stand still, without input).
[ -n "${GAMEPLAY_CLICK:-}" ] && "$tools/keys.sh" click:500:400 wait:500
exit 0
