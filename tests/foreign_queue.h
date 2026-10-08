// A command queue that wraps a real one, like an overlay or capture tool does.
// With `forward_qi` it hands unknown interfaces to the wrapped queue (so a
// layer can see through it); without, it only answers the D3D12 interfaces.
#pragma once

#include "test_util.h"

struct ForeignQueue final : ID3D12CommandQueue {
    ComPtr<ID3D12CommandQueue> inner;
    bool forward_qi;
    ULONG refs = 1;

    ForeignQueue(ID3D12CommandQueue *queue, bool forward) : inner(queue), forward_qi(forward) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild)
            || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue)) {
            *out = static_cast<ID3D12CommandQueue *>(this);
            ++refs;
            return S_OK;
        }
        if (forward_qi)
            return inner->QueryInterface(riid, out);
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs;
        if (n == 0)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override { return inner->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override { return inner->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown *d) override { return inner->SetPrivateDataInterface(g, d); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override { return inner->SetName(n); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void **o) override { return inner->GetDevice(r, o); }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *, UINT, const D3D12_TILED_RESOURCE_COORDINATE *, const D3D12_TILE_REGION_SIZE *, ID3D12Heap *, UINT, const D3D12_TILE_RANGE_FLAGS *, const UINT *, const UINT *, D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *, ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *, const D3D12_TILE_REGION_SIZE *, D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n, ID3D12CommandList *const *l) override { inner->ExecuteCommandLists(n, l); }
    void STDMETHODCALLTYPE SetMarker(UINT, const void *, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void *, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *f, UINT64 v) override { return inner->Signal(f, v); }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *f, UINT64 v) override { return inner->Wait(f, v); }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *f) override { return inner->GetTimestampFrequency(f); }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *a, UINT64 *b) override { return inner->GetClockCalibration(a, b); }
#ifdef _WIN32
    D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC *ret) override { return inner->GetDesc(ret); }
#else
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return inner->GetDesc(); }
#endif
};
