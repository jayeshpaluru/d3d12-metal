#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Launches the game through the running Steam client with the layer installed, and watches it: tails the layer's log
# (build-wine/game-logs/d3d12metal.log) and any log or crash dump the game writes under the prefix's user profile or
# its own folder, and takes window screenshots into build-wine/game-screens/ until the game exits or a timeout.
#
# Usage: tools/run-game.sh [options] [-- game arguments]
#   --direct            run the exe straight under wine instead of through Steam (stand-in games, tests); uses
#                       GAME_WINEPREFIX (default: the test prefix) and needs GAME_DIR
#   --dry-run           print what would be installed and launched, change and start nothing
#   --kill              stop just the game process (pkill by exe name, never wineserver) and exit
#   --no-install        do not run tools/install-game.sh first
#   --timeout SECONDS   stop watching after this long [1800]; Steam mode leaves the game running
#   --start-timeout S   how long to wait for the game process to appear [240 Steam, 60 direct]
#   --screens-every S   seconds between screenshots [15]
# Everything else (and everything after --) is passed to the game; Steam forwards the arguments after the app id.
# Steam must already be running in its prefix: start it with tools/start-steam.sh (this script never starts it).
# Environment: GAME (sm1|sm2, default sm1), GAME_DIR, GAME_EXE, GAME_APPID, GAME_ARGS, STEAM_WINEPREFIX, WINE_ROOT (see tools/game-common.sh),
#              WINDOW_MATCH (regex for the window title to capture, default: any non-Steam Wine window).
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/game-common.sh"

direct=0 kill_only=0 install=1 dry_run_launch=0 timeout=1800 start_timeout="" shot_interval=15
extra=()
read -r -a game_args <<< "${GAME_ARGS:-}"   # the profile's default arguments (Steam mode)
export ROSETTA_ADVERTISE_AVX=1   # Spider-Man 2 needs AVX2 and F16C; Rosetta hides them otherwise (direct mode; Steam games inherit it from tools/start-steam.sh)
while [ $# -gt 0 ]; do
    case "$1" in
        --direct) direct=1; shift ;;
        --dry-run) dry_run_launch=1; shift ;;
        --kill) kill_only=1; shift ;;
        --no-install) install=0; shift ;;
        --timeout) timeout="$2"; shift 2 ;;
        --start-timeout) start_timeout="$2"; shift 2 ;;
        --screens-every) shot_interval="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        --) shift; extra+=("$@"); break ;;
        *) extra+=("$1"); shift ;;
    esac
done

# PIDs of the game: Wine names its processes after the exe (matched exactly, so a shell whose command line merely
# mentions the exe is not mistaken for it).
game_pids() {
    pgrep -x "$GAME_EXE" 2>/dev/null || true
}

if [ "$kill_only" = 1 ]; then
    pids="$(game_pids)"
    if [ -z "$pids" ]; then
        echo "no $GAME_EXE process running"
        exit 0
    fi
    echo "stopping $GAME_EXE: $(echo $pids)"
    kill $pids 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        [ -z "$(game_pids)" ] && { echo "stopped"; exit 0; }
    done
    echo "still running, killing"
    kill -9 $(game_pids) 2>/dev/null
    exit 0
fi

if [ "$direct" = 1 ]; then
    export GAME_WINEPREFIX="${GAME_WINEPREFIX:-${DEPS_DIR:-$HOME/code/deps}/wineprefix}"
    # The point of --direct is to prove the registry overrides and the conf file work without a shell environment.
    unset WINEDLLOVERRIDES WINEDLLPATH
    : "${start_timeout:=60}"
else
    : "${start_timeout:=240}"
fi
need_wine
prefix="$(game_prefix)"
find_game_dir || exit 1

if [ "$direct" = 0 ] && ! pgrep -f 'Steam\\steam.exe' >/dev/null; then
    echo "Steam is not running in $prefix. Start it first (and wait for the library to load):" >&2
    echo "    tools/start-steam.sh" >&2
    exit 2
fi
if [ -n "$(game_pids)" ]; then
    echo "$GAME_EXE is already running (pid $(echo $(game_pids))); use --kill first" >&2
    exit 2
fi

if [ "$dry_run_launch" = 1 ]; then
    [ "$install" = 1 ] && "$game_root/tools/install-game.sh" --dry-run
    if [ "$direct" = 1 ]; then
        echo "dry-run: would launch $game_dir/$GAME_EXE directly (prefix $prefix) ${extra[*]:-}"
    else
        echo "dry-run: would launch app $GAME_APPID through Steam ${extra[*]:-}"
    fi
    exit 0
fi

mkdir -p "$game_logs" "$game_screens"
if [ "$install" = 1 ]; then
    "$game_root/tools/install-game.sh" || exit $?
fi

# Fresh layer log (the previous run's is kept as *.prev).
layer_log="$game_logs/d3d12metal.log"
for f in "$layer_log" "$layer_log.methods"; do
    [ -f "$f" ] && mv -f "$f" "$f.prev"
done
: > "$layer_log"
marker="$(mktemp)"
trap 'rm -f "$marker"' EXIT

# --- window capture ---------------------------------------------------------------------------------------------
lister="$game_root/build-wine/list_windows"
if [ ! -x "$lister" ] || [ "$game_root/tools/list_windows.swift" -nt "$lister" ]; then
    swiftc -O "$game_root/tools/list_windows.swift" -o "$lister" 2>"$game_logs/swiftc.log" || lister=""
fi
shots=0 shot_warned=0
capture() {
    [ -n "$lister" ] || return
    local line id name w h size file
    # A sleeping display cannot be captured: this wakes it (and says the user is active) for a moment.
    caffeinate -u -t 2 2>/dev/null
    # id | owner | title | WxH | onscreen: true | layer: 0
    while IFS= read -r line; do
        id="${line%% |*}"
        name="$(echo "$line" | awk -F' [|] ' '{print $3}')"
        size="$(echo "$line" | awk -F' [|] ' '{print $4}')"
        w="${size%% x*}"; h="${size##*x }"
        [ "${w:-0}" -ge 200 ] 2>/dev/null && [ "${h:-0}" -ge 150 ] 2>/dev/null || continue
        echo "$name" | grep -qi steam && continue
        if [ -n "${WINDOW_MATCH:-}" ]; then echo "$name" | grep -qiE "$WINDOW_MATCH" || continue; fi
        file="$game_screens/$(date +%H%M%S)-$id.png"
        if screencapture -x -o -l "$id" "$file" 2>/dev/null && [ -s "$file" ]; then
            shots=$((shots + 1))
            echo "[screen] $file ($size, \"$name\")"
        else
            rm -f "$file"
            if [ "$shot_warned" = 0 ]; then
                shot_warned=1
                echo "[screen] screencapture failed (display asleep, session locked, or no Screen Recording permission for this terminal)"
            fi
        fi
    done < <("$lister" | grep 'onscreen: true | layer: 0' | grep -i wine)
}

# --- log tailing ------------------------------------------------------------------------------------------------
tail_files=() tail_offsets=() tail_labels=()
watch_file() {
    local f="$1" label="$2" start="${3:-0}" i
    for i in "${!tail_files[@]}"; do [ "${tail_files[$i]}" = "$f" ] && return; done
    tail_files+=("$f"); tail_labels+=("$label"); tail_offsets+=("$start")
    echo "[watch] $f"
}
drain_files() {
    local i f size off cap shown
    for i in "${!tail_files[@]}"; do
        f="${tail_files[$i]}"
        [ -f "$f" ] || continue
        size="$(stat -f %z "$f")"
        off="${tail_offsets[$i]}"
        if [ "$size" -lt "$off" ]; then off=0; fi   # truncated: start over
        [ "$size" -gt "$off" ] || continue
        case "$f" in *.dmp|*.mdmp|*.dump) echo "[${tail_labels[$i]}] $size bytes (binary)"; tail_offsets[$i]="$size"; continue ;; esac
        shown=0 cap=60
        while IFS= read -r line; do
            shown=$((shown + 1))
            [ "$shown" -le "$cap" ] && echo "[${tail_labels[$i]}] $line"
        done < <(tail -c +$((off + 1)) "$f" | head -c $((size - off)) | tr -d '\r')
        [ "$shown" -gt "$cap" ] && echo "[${tail_labels[$i]}] ... $((shown - cap)) more lines in $f"
        tail_offsets[$i]="$size"
    done
}
# Logs and dumps the game wrote since launch: the user profile of the prefix (minus Steam's own) and the game folder.
discover() {
    local f
    while IFS= read -r f; do
        watch_file "$f" "$(basename "$f")"
    done < <({ find "$prefix/drive_c/users" -type f \( -name '*.log' -o -name '*.txt' -o -name '*.dmp' -o -name '*.mdmp' -o -name '*.dump' \) -newer "$marker" 2>/dev/null \
                   | grep -viE '/(steam|htmlcache|cef|crashhandler)/|steamwebhelper|/Temp/|/Wine/'
               find "$game_dir" -type f \( -name '*.log' -o -name '*.txt' -o -name '*.dmp' -o -name '*.mdmp' -o -name '*.dump' \) -newer "$marker" 2>/dev/null; } | head -12)
}

# --- launch ---------------------------------------------------------------------------------------------------------
touch "$marker"
watch_file "$layer_log" layer
launcher_log="$game_logs/launch.log"
if [ "$direct" = 1 ]; then
    echo "launching $game_dir/$GAME_EXE directly (prefix $prefix) ${extra[*]:-}"
    ( cd "$game_dir" && exec env WINEPREFIX="$prefix" WINEDEBUG="${WINEDEBUG:--all}" "$WINE_ROOT/bin/wine" "$GAME_EXE" ${extra[@]+"${extra[@]}"} ) >"$launcher_log" 2>&1 &
else
    echo "launching app $GAME_APPID through Steam ${extra[*]:-}"
    WINEPREFIX="$prefix" WINEDEBUG="${WINEDEBUG:--all}" "$WINE_ROOT/bin/wine" 'C:\Program Files (x86)\Steam\steam.exe' \
        -applaunch "$GAME_APPID" ${game_args[@]+"${game_args[@]}"} ${extra[@]+"${extra[@]}"} >"$launcher_log" 2>&1 &
fi
launcher=$!

start=$SECONDS
seen=0 last_shot=-999 last_discover=0 status=timeout
while :; do
    elapsed=$((SECONDS - start))
    pids="$(game_pids)"
    if [ -n "$pids" ]; then
        if [ "$seen" = 0 ]; then
            seen=1
            echo "[run] game process up after ${elapsed}s: $(echo $pids)"
        fi
    elif [ "$seen" = 1 ]; then
        status=exited
        break
    elif [ "$elapsed" -ge "$start_timeout" ]; then
        status=never-started
        break
    fi
    if [ "$elapsed" -ge "$timeout" ]; then
        break
    fi
    if [ "$elapsed" -ge $((last_discover + 5)) ]; then
        discover
        last_discover=$elapsed
    fi
    drain_files
    if [ "$seen" = 1 ] && [ "$elapsed" -ge $((last_shot + shot_interval)) ]; then
        capture
        last_shot=$elapsed
    fi
    sleep 2
done
sleep 1
discover
drain_files

# --- summary --------------------------------------------------------------------------------------------------------
echo
echo "== $status after $((SECONDS - start))s"
case "$status" in
    never-started)
        echo "$GAME_EXE did not start within ${start_timeout}s; see $launcher_log (and Steam's window). Its last lines:"
        tail -n 15 "$launcher_log" | sed 's/^/    /' ;;
    timeout)
        if [ "$direct" = 1 ]; then
            echo "stopping the game (timeout)"
            "$0" --kill >/dev/null
        else
            echo "the game is still running; stop it with: tools/run-game.sh --kill"
        fi ;;
esac
echo "layer log:     $layer_log ($(wc -l < "$layer_log" | tr -d ' ') lines)"
[ -f "$layer_log.methods" ] && echo "method list:   $layer_log.methods ($(($(wc -l < "$layer_log.methods") - 1)) distinct methods)"
echo "screenshots:   $shots in $game_screens"
for f in ${tail_files[@]+"${tail_files[@]}"}; do
    case "$f" in "$layer_log") ;; *) echo "game file:     $f ($(stat -f %z "$f") bytes)" ;; esac
done
grep -v ' trace t[0-9]' "$layer_log" 2>/dev/null | grep -iE "not implemented|failed|skipped|invalid" | sort | uniq -c | sort -rn | head -8 | sed 's/^/problem:       /'

exit 0
