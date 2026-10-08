// Makes __uuidof() work for the D3D12 interfaces.
//
// DirectX-Headers declares the interface IDs as IID_* constants (defined in
// libDirectX-Guids) but does not attach them to the interface types, so
// __uuidof(ID3D12Device) and IID_PPV_ARGS(&device) would not compile.
#pragma once

// d3d12.h needs IUnknown to be declared first with this header set.
#include <unknwn.h>

#include <directx/d3d12.h>

#define D3D12M_DECLARE_UUID(type)                                        \
    extern "C++" {                                                       \
    template <>                                                          \
    constexpr const GUID &__wsl_stub_uuidof<type>() { return IID_##type; }  \
    template <>                                                          \
    constexpr const GUID &__wsl_stub_uuidof<type *>() { return IID_##type; } \
    }

D3D12M_DECLARE_UUID(ID3D12Object)
D3D12M_DECLARE_UUID(ID3D12DeviceChild)
D3D12M_DECLARE_UUID(ID3D12Pageable)
D3D12M_DECLARE_UUID(ID3D12Device)
D3D12M_DECLARE_UUID(ID3D12Device1)
D3D12M_DECLARE_UUID(ID3D12Device2)
D3D12M_DECLARE_UUID(ID3D12CommandQueue)
D3D12M_DECLARE_UUID(ID3D12CommandAllocator)
D3D12M_DECLARE_UUID(ID3D12CommandList)
D3D12M_DECLARE_UUID(ID3D12GraphicsCommandList)
D3D12M_DECLARE_UUID(ID3D12GraphicsCommandList1)
D3D12M_DECLARE_UUID(ID3D12Resource)
D3D12M_DECLARE_UUID(ID3D12Fence)
D3D12M_DECLARE_UUID(ID3D12DescriptorHeap)
D3D12M_DECLARE_UUID(ID3D12PipelineState)
D3D12M_DECLARE_UUID(ID3D12RootSignature)
D3D12M_DECLARE_UUID(ID3D12RootSignatureDeserializer)
D3D12M_DECLARE_UUID(ID3D12VersionedRootSignatureDeserializer)
D3D12M_DECLARE_UUID(ID3D12Heap)
D3D12M_DECLARE_UUID(ID3D10Blob)

#undef D3D12M_DECLARE_UUID
