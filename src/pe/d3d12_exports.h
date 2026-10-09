// SPDX-License-Identifier: LGPL-2.1-or-later
/* The DXGI entry points exported by d3d12.dll (src/pe/d3d12.def), for dxgi_forwarder.c. */
#pragma once

#include <windows.h>

__declspec(dllimport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **factory);
__declspec(dllimport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **factory);
__declspec(dllimport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **factory);
__declspec(dllimport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void **debug);
__declspec(dllimport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void);
