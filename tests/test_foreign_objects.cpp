// Objects that are not this layer's (wrappers, proxies, MSVC-built objects)
// reach the API: they must be refused or seen through, never downcast blindly.
#include "foreign_queue.h"
#include "test_context.h"

namespace {

// A fence that is not ours.
struct ForeignFence : ID3D12Fence {
    ULONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Fence) || riid == __uuidof(ID3D12Object)
            || riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable)) {
            *out = static_cast<ID3D12Fence *>(this);
            ++refs;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override { return E_FAIL; }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return 0; }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override { return E_FAIL; }
};

} // namespace

int main()
{
    TestContext ctx;

    // A wrapper that forwards unknown interfaces is seen through; one that does not is refused.
    ForeignQueue *forwarding = new ForeignQueue(ctx.queue.Get(), true);
    ForeignQueue *opaque = new ForeignQueue(ctx.queue.Get(), false);
    ComPtr<ID3D12CommandQueue> forwarding_queue, opaque_queue;
    forwarding_queue.Attach(forwarding);
    opaque_queue.Attach(opaque);

    ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
    CHECK_HR(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    forwarding_queue->ExecuteCommandLists(1, lists);
    CHECK_HR(ctx.queue->Signal(ctx.fence.Get(), 1));
    CHECK_HR(ctx.fence->SetEventOnCompletion(1, nullptr));

    // Foreign objects given to our queue, lists and device calls are refused without crashing.
    ForeignFence foreign_fence;
    CHECK(ctx.queue->Signal(&foreign_fence, 1) == E_INVALIDARG);
    CHECK(ctx.queue->Wait(&foreign_fence, 1) == E_INVALIDARG);
    CHECK(forwarding_queue->Signal(&foreign_fence, 1) == E_INVALIDARG);

    ComPtr<ID3D12GraphicsCommandList> recording = ctx.create_list();
    ComPtr<ID3D12Resource> buffer = ctx.create_buffer(D3D12_HEAP_TYPE_DEFAULT, 64);
    recording->CopyBufferRegion(buffer.Get(), 0, reinterpret_cast<ID3D12Resource *>(&foreign_fence), 0, 16);
    recording->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(&foreign_fence));
    recording->SetGraphicsRootSignature(reinterpret_cast<ID3D12RootSignature *>(&foreign_fence));
    CHECK_HR(recording->Close());

    // A foreign object in a list submission is skipped (the submission is refused).
    ComPtr<ID3D12CommandList> as_list;
    CHECK_HR(recording->QueryInterface(IID_PPV_ARGS(as_list.ReleaseAndGetAddressOf())));
    ID3D12CommandList *mixed[] = {reinterpret_cast<ID3D12CommandList *>(&foreign_fence)};
    ctx.queue->ExecuteCommandLists(1, mixed);
    ctx.wait_idle();
    return 0;
}
