// SPDX-License-Identifier: LGPL-2.1-or-later
// Replaces steamwebhelper.exe: runs steamwebhelper_real.exe with --in-process-gpu appended (see tools/start-steam.sh).
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
