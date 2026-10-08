// Makes __uuidof() work for the D3D12 interfaces.
//
// DirectX-Headers declares the interface IDs as IID_* constants (defined in
// libDirectX-Guids) but does not attach them to the interface types, so
// __uuidof(ID3D12Device) and IID_PPV_ARGS(&device) would not compile.
// dxguids.h attaches them (through __CRT_UUID_DECL of the wsl stubs).
#pragma once

#ifdef _WIN32
// The MinGW d3d12.h: unlike DirectX-Headers it declares the interface IDs
// (__uuidof works) and the struct-returning methods (GetDesc and friends) in
// the Windows x64 form, which GCC does not generate for a plain by-value return.
#include <windows.h>
#include <d3d12.h>

// Only in DirectX-Headers' d3dcommon.h.
static const GUID WKPDID_D3DDebugObjectNameW = {0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};
#else

// d3d12.h needs IUnknown to be declared first with this header set.
#include <unknwn.h>

#include <directx/d3d12.h>

// Must follow the headers whose interfaces it declares.
#include <dxguids/dxguids.h>

// dxguids.h has no entry for the blob interface.
WINADAPTER_IID(ID3D10Blob, 0x8ba5fb08, 0x5195, 0x40e2, 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02);

#endif // _WIN32
