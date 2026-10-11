// SPDX-License-Identifier: LGPL-2.1-or-later
// Swap chain behaviour on a real Wine window: back buffer cycling, the frame
// latency waitable object (real Win32 event), ResizeBuffers with new sizes and
// formats (also with zero size = the client area), outputs and display modes.
#include <cstring>

#include "wine_test.h"

namespace {

void pump_messages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND create_window(UINT width, UINT height)
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"SwapChainTest";
    RegisterClassExW(&window_class);
    RECT rect = {0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowW(window_class.lpszClassName, L"swapchain_test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                window_class.hInstance, nullptr);
    CHECK(window);
    ShowWindow(window, SW_SHOWDEFAULT);
    pump_messages();
    return window;
}

struct Renderer {
    DeviceContext &ctx;
    IDXGISwapChain3 *swap_chain;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;

    Renderer(DeviceContext &c, IDXGISwapChain3 *sc) : ctx(c), swap_chain(sc)
    {
        CHECK_HR(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        CHECK_HR(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&list)));
        CHECK_HR(list->Close());
    }

    // Clears the current back buffer and presents it; waits for the GPU.
    void frame(const float color[4], UINT sync_interval)
    {
        DXGI_SWAP_CHAIN_DESC1 desc;
        CHECK_HR(swap_chain->GetDesc1(&desc));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = desc.BufferCount;
        rtv_heap.Reset();
        CHECK_HR(ctx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));

        const UINT index = swap_chain->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> buffer;
        CHECK_HR(swap_chain->GetBuffer(index, IID_PPV_ARGS(&buffer)));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(index) * ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        ctx.device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);

        CHECK_HR(allocator->Reset());
        CHECK_HR(list->Reset(allocator.Get(), nullptr));
        list->ClearRenderTargetView(rtv, color, 0, nullptr);
        CHECK_HR(list->Close());
        ID3D12CommandList *lists[] = {list.Get()};
        ctx.queue->ExecuteCommandLists(1, lists);
        CHECK_HR(swap_chain->Present(sync_interval, 0));
        ctx.wait_idle();
        pump_messages();
    }
};

} // namespace

int main()
{
    constexpr UINT kWidth = 640, kHeight = 360;
    HWND window = create_window(kWidth, kHeight);
    DeviceContext ctx;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> swap_chain1;
    CHECK_HR(ctx.factory->CreateSwapChainForHwnd(ctx.queue.Get(), window, &desc, nullptr, nullptr, &swap_chain1));
    ComPtr<IDXGISwapChain3> swap_chain;
    CHECK_HR(swap_chain1.As(&swap_chain));
    std::printf("swapchain_test: created\n");

    HWND associated = nullptr;
    CHECK_HR(swap_chain1->GetHwnd(&associated));
    CHECK(associated == window);

    // Frame latency: the waitable object is signaled at the start and again each
    // time a presented frame has finished.
    ComPtr<IDXGISwapChain2> swap_chain2;
    CHECK_HR(swap_chain1.As(&swap_chain2));
    HANDLE waitable = swap_chain2->GetFrameLatencyWaitableObject();
    CHECK(waitable);
    CHECK_HR(swap_chain2->SetMaximumFrameLatency(1));
    UINT latency = 0;
    CHECK_HR(swap_chain2->GetMaximumFrameLatency(&latency));
    CHECK(latency == 1);

    Renderer renderer(ctx, swap_chain.Get());
    const float colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    for (UINT i = 0; i < 9; ++i) {
        CHECK(WaitForSingleObject(waitable, 2000) == WAIT_OBJECT_0);
        CHECK(swap_chain->GetCurrentBackBufferIndex() == i % 3);
        renderer.frame(colors[i % 3], 1);
    }
    CHECK(WaitForSingleObject(waitable, 2000) == WAIT_OBJECT_0);
    CloseHandle(waitable);
    std::printf("swapchain_test: frame latency OK\n");

    // Resize to other sizes and formats; zero size means the client area.
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
    UINT width = 800;
    for (DXGI_FORMAT format : formats) {
        CHECK_HR(swap_chain->ResizeBuffers(2, width, 450, format, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
        DXGI_SWAP_CHAIN_DESC1 got;
        CHECK_HR(swap_chain->GetDesc1(&got));
        CHECK(got.Width == width && got.Height == 450 && got.BufferCount == 2 && got.Format == format);
        for (int i = 0; i < 3; ++i)
            renderer.frame(colors[i], 1);
        width += 32;
    }
    CHECK_HR(swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
    DXGI_SWAP_CHAIN_DESC1 got;
    CHECK_HR(swap_chain->GetDesc1(&got));
    RECT client;
    GetClientRect(window, &client);
    CHECK(got.Width == UINT(client.right - client.left) && got.Height == UINT(client.bottom - client.top));
    renderer.frame(colors[0], 0);
    std::printf("swapchain_test: resize OK (client %ldx%ld)\n", client.right - client.left, client.bottom - client.top);

    // Outputs and display modes come from the Windows side of Wine.
    ComPtr<IDXGIOutput> output;
    CHECK_HR(swap_chain->GetContainingOutput(&output));
    DXGI_OUTPUT_DESC output_desc;
    CHECK_HR(output->GetDesc(&output_desc));
    UINT modes = 0;
    CHECK_HR(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modes, nullptr));
    CHECK(modes > 0);
    std::printf("swapchain_test: output %ldx%ld, %u display modes\n",
                output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left,
                output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top, modes);

    // Swap chains come and go on one window: the first and last cycle create the
    // window's Metal view, the ones in between share it. Every one must present.
    {
        ComPtr<IDXGISwapChain1> extra;
        CHECK_HR(ctx.factory->CreateSwapChainForHwnd(ctx.queue.Get(), window, &desc, nullptr, nullptr, &extra));
        swap_chain.Reset();
        swap_chain2.Reset();
        swap_chain1.Reset();  // the first swap chain, which created the view, goes before `extra`
        ComPtr<IDXGISwapChain3> extra3;
        CHECK_HR(extra.As(&extra3));
        Renderer extra_renderer(ctx, extra3.Get());
        extra_renderer.frame(colors[1], 1);
    }
    for (int i = 0; i < 20; ++i) {
        DXGI_SWAP_CHAIN_DESC1 cycle_desc = desc;
        cycle_desc.Flags = 0;
        cycle_desc.BufferCount = 2 + i % 2;
        ComPtr<IDXGISwapChain1> cycle;
        CHECK_HR(ctx.factory->CreateSwapChainForHwnd(ctx.queue.Get(), window, &cycle_desc, nullptr, nullptr, &cycle));
        ComPtr<IDXGISwapChain3> cycle3;
        CHECK_HR(cycle.As(&cycle3));
        Renderer cycle_renderer(ctx, cycle3.Get());
        cycle_renderer.frame(colors[i % 3], 1);
    }
    std::printf("swapchain_test: create/destroy cycles OK\n");
    DestroyWindow(window);
    std::printf("swapchain_test: PASS\n");
    return 0;
}
