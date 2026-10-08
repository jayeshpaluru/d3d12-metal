#!/bin/bash
# Prints the state of the game's screen: profile1, profile2 (SELECT PROFILE, that row highlighted), main-continue (main menu, CONTINUE
# highlighted), gameplay (the open world of the saved scene), or unknown (intro, loading, a menu without a reference, no window).
# Compares a capture with the reference captures of the game profile in build-wine/ref/<sm1|sm2>/ (<state>.png,
# <state>.<n>.png; made locally with `build-wine/classify --save <png> build-wine/ref/<game>/<state>.png`, never committed). A capture can fail or catch a fade, so an
# unknown screen is looked at twice more. usage: GAME=sm1|sm2 tools/game/state.sh
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
classify="$game_root/build-wine/classify"
[ -x "$classify" ] || swiftc -O -o "$classify" "$game_root/tools/game/classify.swift" || exit 1
for attempt in 1 2 3; do
    state=unknown
    if shot="$("$game_root/tools/game/screenshot.sh" state 2>/dev/null)"; then
        state="$("$classify" "$shot" "$game_ref" | awk '{print $1}')"
    fi
    [ "$state" != unknown ] && break
    sleep 1
done
echo "$state"
