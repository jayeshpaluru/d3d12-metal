/* dxgi.dll has no logic of its own: each export calls the same function in d3d12.dll, which carries the whole
 * layer. They are real functions, not export forwarders (a .def "name = d3d12.name" entry): code that reads the
 * export table itself, as the AMD GPU Services library does, takes a forwarder's RVA for a function and jumps into
 * the table of strings. */
#include <windows.h>

#include "d3d12_exports.h"

HRESULT WINAPI dxgi_CreateDXGIFactory(REFIID riid, void **factory)
{
    return CreateDXGIFactory(riid, factory);
}

HRESULT WINAPI dxgi_CreateDXGIFactory1(REFIID riid, void **factory)
{
    return CreateDXGIFactory1(riid, factory);
}

HRESULT WINAPI dxgi_CreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
    return CreateDXGIFactory2(flags, riid, factory);
}

HRESULT WINAPI dxgi_DXGIGetDebugInterface1(UINT flags, REFIID riid, void **debug)
{
    return DXGIGetDebugInterface1(flags, riid, debug);
}

HRESULT WINAPI dxgi_DXGIDeclareAdapterRemovalSupport(void)
{
    return DXGIDeclareAdapterRemovalSupport();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
