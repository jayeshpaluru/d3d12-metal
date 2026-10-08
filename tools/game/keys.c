/* Win32 input helper: run inside the game's Wine prefix, needs no macOS permissions.
 * usage: wine keys.exe [-w <title substring>] [-m sendinput|post] <step>...
 * steps: enter esc space tab up down left right f1..f12 a..z 0..9 (one key press), hold:<key>:<ms>,
 *        click:<x>:<y> (client coordinates), wait:<ms>, focus (bring the window to the foreground), list (print windows)
 * Default is SendInput with scan codes (what DirectInput/raw input readers see); -m post sends WM_KEYDOWN/UP to the window. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *want = "Spider-Man";
static HWND found;
static int use_post;

static BOOL CALLBACK enum_cb(HWND h, LPARAM list)
{
    char t[256];
    DWORD pid;
    if (!IsWindowVisible(h)) return TRUE;
    GetWindowTextA(h, t, sizeof t);
    if (list) { GetWindowThreadProcessId(h, &pid); printf("%p pid %lu '%s'\n", (void *)h, pid, t); }
    else if (t[0] && strstr(t, want)) { found = h; return FALSE; }
    return TRUE;
}

static WORD vk_for(const char *k)
{
    static const struct { const char *n; WORD vk; } m[] = {
        {"enter", VK_RETURN}, {"esc", VK_ESCAPE}, {"space", VK_SPACE}, {"tab", VK_TAB}, {"up", VK_UP}, {"down", VK_DOWN},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"backspace", VK_BACK},
    };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++) if (!strcmp(k, m[i].n)) return m[i].vk;
    if (k[0] == 'f' && k[1]) return (WORD)(VK_F1 + atoi(k + 1) - 1);
    if (k[0] >= 'a' && k[0] <= 'z' && !k[1]) return (WORD)(k[0] - 'a' + 'A');
    if (k[0] >= '0' && k[0] <= '9' && !k[1]) return (WORD)k[0];
    fprintf(stderr, "unknown key %s\n", k);
    exit(2);
}

static void key(WORD vk, int down)
{
    if (use_post) {
        UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        PostMessageA(found, down ? WM_KEYDOWN : WM_KEYUP, vk, 1 | (sc << 16) | (down ? 0 : 0xC0000000));
        return;
    }
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof in);
}

static void click(int x, int y)
{
    POINT p = {x, y};
    ClientToScreen(found, &p);
    SetCursorPos(p.x, p.y);
    Sleep(50);
    INPUT in[2] = {{0}};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof in[0]);
    Sleep(60);
    SendInput(1, &in[1], sizeof in[1]);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (!strcmp(argv[i], "-w")) want = argv[i + 1];
        else if (!strcmp(argv[i], "-m")) use_post = !strcmp(argv[i + 1], "post");
    }
    if (i < argc && !strcmp(argv[i], "list")) { EnumWindows(enum_cb, 1); return 0; }
    EnumWindows(enum_cb, 0);
    if (!found) { fprintf(stderr, "no window matching '%s'\n", want); return 1; }
    SetForegroundWindow(found);
    Sleep(200);
    for (; i < argc; i++) {
        const char *s = argv[i];
        if (!strncmp(s, "wait:", 5)) Sleep((DWORD)atoi(s + 5));
        else if (!strcmp(s, "focus")) { SetForegroundWindow(found); Sleep(200); }
        else if (!strncmp(s, "hold:", 5)) {
            char k[32]; int ms = 0;
            if (sscanf(s + 5, "%31[^:]:%d", k, &ms) < 2) return 2;
            WORD vk = vk_for(k);
            key(vk, 1); Sleep((DWORD)ms); key(vk, 0);
        } else if (!strncmp(s, "click:", 6)) {
            int x, y;
            if (sscanf(s + 6, "%d:%d", &x, &y) != 2) return 2;
            click(x, y);
        } else {
            WORD vk = vk_for(s);
            key(vk, 1); Sleep(80); key(vk, 0); Sleep(250);
        }
    }
    return 0;
}
