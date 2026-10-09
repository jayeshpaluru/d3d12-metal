#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Launch the Windows Steam client in its own Wine prefix (kept separate from the
# test prefix, whose wineserver the test scripts kill).
#   WINE_ROOT             directory with bin/wine [the Gcenx Wine Devel app in deps]
#   STEAM_WINEPREFIX      prefix with Steam installed [deps/wineprefix-steam]
#   STEAM_CEF_FLAGS       extra steam.exe flags ["-cef-disable-gpu"]
#   STEAMWEBHELPER_EXTRA  Chromium switches the webhelper shim appends ["--in-process-gpu"]
# Pass --restart to stop everything running in that prefix first.
#
# Steam's UI windows stay black under winemac because Chromium's GPU process
# draws into HWNDs owned by the browser process, and Wine's mac driver can't
# present into another process's window. Steam doesn't forward
# --in-process-gpu, so steamwebhelper.exe is replaced by a tiny shim that runs
# the real binary (steamwebhelper_real.exe) with that switch added. The shim is
# built with mingw on first use and reinstalled whenever a Steam update
# restores the real steamwebhelper.exe.
set -euo pipefail
WINE_ROOT="${WINE_ROOT:-${DEPS_DIR:-$HOME/code/deps}/wine/Wine Devel.app/Contents/Resources/wine}"
export WINEPREFIX="${STEAM_WINEPREFIX:-${DEPS_DIR:-$HOME/code/deps}/wineprefix-steam}"
export WINEDEBUG="${WINEDEBUG:--all}"
# Rosetta hides AVX/AVX2/FMA/F16C from x86 code unless asked; Spider-Man 2
# requires AVX2 and F16C. Games launched by Steam inherit this.
export ROSETTA_ADVERTISE_AVX="${ROSETTA_ADVERTISE_AVX:-1}"
if [ "${1:-}" = "--restart" ]; then
    shift
    "$WINE_ROOT/bin/wineserver" -k || true
    "$WINE_ROOT/bin/wineserver" -w || true
fi

shim="$WINEPREFIX/steamwebhelper-shim.exe"
cef="$WINEPREFIX/drive_c/Program Files (x86)/Steam/bin/cef/cef.win64"
if [ ! -f "$shim" ]; then
    x86_64-w64-mingw32-gcc -O2 -municode -mwindows -x c -o "$shim" - <<'EOF'
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR unused, int show)
{
    static wchar_t exe[MAX_PATH], extra[1024], cmd[32768];
    const wchar_t *args = GetCommandLineW();
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy(wcsrchr(exe, L'\\') + 1, L"steamwebhelper_real.exe");
    if (!GetEnvironmentVariableW(L"STEAMWEBHELPER_EXTRA", extra, 1024))
        wcscpy(extra, L"--in-process-gpu");
    if (*args == L'"') { args++; while (*args && *args != L'"') args++; if (*args) args++; }
    else while (*args && *args != L' ' && *args != L'\t') args++;
    _snwprintf(cmd, 32767, L"\"%ls\"%ls %ls", exe, args, extra);
    GetStartupInfoW(&si);
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    return code;
}
EOF
fi
if ! cmp -s "$shim" "$cef/steamwebhelper.exe"; then
    cp "$cef/steamwebhelper.exe" "$cef/steamwebhelper_real.exe"
    cp "$shim" "$cef/steamwebhelper.exe"
fi

exec "$WINE_ROOT/bin/wine" 'C:\Program Files (x86)\Steam\steam.exe' -no-cef-sandbox -noverifyfiles ${STEAM_CEF_FLAGS--cef-disable-gpu} "$@"
