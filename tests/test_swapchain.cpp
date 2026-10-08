// Swap chains without a window: a detached CAMetalLayer stands in for the window
// (the Wine build looks the layer up in the Wine window instead). Covers the
// DXGI object (back buffers, index cycling, resize, format changes, legacy
// creation, outputs) and the present pass, whose output is checked through the
// backend's present dump: an RGBA back buffer must reach the BGRA layer with
// its channels in the right place.
#include <cstdlib>
#include <string>

#include "swapchain_support.h"
#include "test_context.h"

namespace {

struct Frame {
    TestContext &ctx;
    IDXGISwapChain3 *swap_chain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;

    // Clears the current back buffer to `color` and presents it.
    void present(const float color[4], UINT sync_interval)
    {
        const UINT count = [&] {
            DXGI_SWAP_CHAIN_DESC1 desc;
            CHECK_HR(swap_chain->GetDesc1(&desc));
            return desc.BufferCount;
        }();
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = count;
        CHECK_HR(ctx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap.ReleaseAndGetAddressOf())));
        const UINT increment = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        const UINT index = swap_chain->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> buffer;
        CHECK_HR(swap_chain->GetBuffer(index, IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(index) * increment;
        ctx.device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);

        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        list->ClearRenderTargetView(rtv, color, 0, nullptr);
        CHECK_HR(list->Close());
        ID3D12CommandList *lists[] = {list.Get()};
        ctx.queue->ExecuteCommandLists(1, lists);
        CHECK_HR(swap_chain->Present(sync_interval, 0));
        ctx.wait_idle();
    }
};

} // namespace

int main()
{
    test_install_layer_provider();
    const std::string dump = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/d3d12metal_test_swapchain.png";
    std::remove(dump.c_str());
    setenv("D3D12METAL_DUMP_PRESENT", dump.c_str(), 1);
    setenv("D3D12METAL_DUMP_PRESENT_FRAME", "3", 1);

    TestContext ctx;
    ComPtr<IDXGIFactory4> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())));

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 64;
    desc.Height = 48;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;

    // Not a command queue of this layer: refused.
    ComPtr<IDXGISwapChain1> swap_chain1;
    CHECK(factory->CreateSwapChainForHwnd(ctx.device.Get(), (HWND)1, &desc, nullptr, nullptr,
                                          swap_chain1.ReleaseAndGetAddressOf()) == DXGI_ERROR_INVALID_CALL);
    CHECK_HR(factory->CreateSwapChainForHwnd(ctx.queue.Get(), (HWND)1, &desc, nullptr, nullptr,
                                             swap_chain1.ReleaseAndGetAddressOf()));
    ComPtr<IDXGISwapChain3> swap_chain;
    CHECK_HR(swap_chain1.As(&swap_chain));
    ComPtr<IDXGISwapChain4> swap_chain4;
    CHECK_HR(swap_chain1.As(&swap_chain4));

    DXGI_SWAP_CHAIN_DESC1 got = {};
    CHECK_HR(swap_chain->GetDesc1(&got));
    CHECK(got.Width == 64 && got.Height == 48 && got.BufferCount == 2 && got.Format == DXGI_FORMAT_R8G8B8A8_UNORM);

    // The device of the swap chain is its queue.
    ComPtr<ID3D12CommandQueue> queue_again;
    CHECK_HR(swap_chain->GetDevice(IID_PPV_ARGS(queue_again.ReleaseAndGetAddressOf())));
    CHECK(queue_again.Get() == ctx.queue.Get());
    ComPtr<IDXGIFactory> parent;
    CHECK_HR(swap_chain->GetParent(IID_PPV_ARGS(parent.ReleaseAndGetAddressOf())));

    // Back buffers are textures of the swap chain's size and format, and the
    // index moves on at each present (flip model).
    ComPtr<ID3D12Resource> buffer;
    CHECK_HR(swap_chain->GetBuffer(1, IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())));
    const D3D12_RESOURCE_DESC buffer_desc = buffer->GetDesc();
    CHECK(buffer_desc.Width == 64 && buffer_desc.Height == 48 && buffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(swap_chain->GetBuffer(2, IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())) == DXGI_ERROR_INVALID_CALL);
    buffer.Reset();

    Frame frame{ctx, swap_chain.Get(), {}};
    const float red_blue[4] = {1.0f, 0.0f, 0.5f, 1.0f};
    for (UINT i = 0; i < 4; ++i) {
        CHECK(swap_chain->GetCurrentBackBufferIndex() == i % 2);
        frame.present(red_blue, i % 2);  // alternates immediate and vsynced presents
    }
    CHECK_HR(swap_chain->Present(0, DXGI_PRESENT_TEST));

    // The third present went to the dump: R and B must not be swapped.
    uint8_t rgb[3] = {};
    CHECK(test_png_pixel(dump.c_str(), 8, 8, rgb));
    std::printf("presented pixel: (%u,%u,%u)\n", rgb[0], rgb[1], rgb[2]);
    CHECK(rgb[0] > 250 && rgb[1] < 5 && rgb[2] > 123 && rgb[2] < 133);

    // Resize, including format changes the layer has to follow.
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    UINT width = 96;
    for (DXGI_FORMAT format : formats) {
        CHECK_HR(swap_chain->ResizeBuffers(3, width, 80, format, 0));
        CHECK_HR(swap_chain->GetDesc1(&got));
        CHECK(got.Width == width && got.Height == 80 && got.BufferCount == 3 && got.Format == format);
        CHECK(swap_chain->GetCurrentBackBufferIndex() == 0);
        CHECK_HR(swap_chain->GetBuffer(2, IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())));
        CHECK(buffer->GetDesc().Format == format && buffer->GetDesc().Width == width);
        buffer.Reset();
        frame.present(red_blue, 0);
        frame.present(red_blue, 0);
        width += 16;
    }

    // Fullscreen, colour space and frame latency are accepted or refused sensibly.
    BOOL fullscreen = TRUE;
    CHECK_HR(swap_chain->SetFullscreenState(FALSE, nullptr));
    CHECK_HR(swap_chain->GetFullscreenState(&fullscreen, nullptr));
    CHECK(!fullscreen);
    UINT support = 0;
    CHECK_HR(swap_chain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &support));
    CHECK(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT);
    CHECK_HR(swap_chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
    CHECK(swap_chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) == E_INVALIDARG);

    // The outputs: one, with a display mode list.
    ComPtr<IDXGIOutput> output;
    CHECK_HR(swap_chain->GetContainingOutput(output.ReleaseAndGetAddressOf()));
    UINT modes = 0;
    CHECK_HR(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modes, nullptr));
    CHECK(modes > 0);
    ComPtr<IDXGIAdapter1> adapter;
    CHECK_HR(factory->EnumAdapters1(0, adapter.ReleaseAndGetAddressOf()));
    ComPtr<IDXGIOutput> first_output, second_output;
    CHECK_HR(adapter->EnumOutputs(0, first_output.ReleaseAndGetAddressOf()));
    CHECK(adapter->EnumOutputs(1, second_output.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND);

    // The legacy creation call and description.
    DXGI_SWAP_CHAIN_DESC legacy = {};
    legacy.BufferDesc.Width = 32;
    legacy.BufferDesc.Height = 32;
    legacy.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    legacy.SampleDesc.Count = 1;
    legacy.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    legacy.BufferCount = 2;
    legacy.OutputWindow = (HWND)1;
    legacy.Windowed = TRUE;
    legacy.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    ComPtr<IDXGISwapChain> legacy_chain;
    CHECK_HR(factory->CreateSwapChain(ctx.queue.Get(), &legacy, legacy_chain.ReleaseAndGetAddressOf()));
    DXGI_SWAP_CHAIN_DESC legacy_got = {};
    CHECK_HR(legacy_chain->GetDesc(&legacy_got));
    CHECK(legacy_got.BufferDesc.Width == 32 && legacy_got.BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
          && legacy_got.OutputWindow == (HWND)1 && legacy_got.Windowed);
    CHECK_HR(legacy_chain->Present(1, 0));
    ctx.wait_idle();

    // Zero buffers or sizes fail instead of creating something broken.
    DXGI_SWAP_CHAIN_DESC1 bad = desc;
    bad.Width = 0;
    bad.Height = 0;
    ComPtr<IDXGISwapChain1> none;
    CHECK(factory->CreateSwapChainForHwnd(ctx.queue.Get(), (HWND)1, &bad, nullptr, nullptr,
                                          none.ReleaseAndGetAddressOf()) == DXGI_ERROR_INVALID_CALL);
    return 0;
}
