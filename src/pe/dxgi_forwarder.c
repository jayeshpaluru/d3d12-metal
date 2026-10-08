/* dxgi.dll has no code: its exports (dxgi.def) are forwarded to d3d12.dll, which
 * carries the whole layer. */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
