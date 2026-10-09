// SPDX-License-Identifier: LGPL-2.1-or-later
// The performance overlay (D3D12METAL_HUD=1): presents a black back buffer through a detached layer and checks, on the
// backend's present dump, that the top-left corner holds white text pixels with a dark shadow and the rest of the
// frame is untouched. Run twice by meson: with the overlay and without (nothing drawn).
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "swapchain_support.h"
#include "test_context.h"

int main(int argc, char **argv)
{
    const bool expect_hud = argc > 1 && std::string(argv[1]) == "on";
    test_install_layer_provider();
    const std::string dump = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/d3d12metal_test_hud_" + (expect_hud ? "on" : "off") + ".png";
    std::remove(dump.c_str());
    setenv("D3D12METAL_DUMP_PRESENT", dump.c_str(), 1);
    setenv("D3D12METAL_DUMP_PRESENT_FRAME", "3", 1);
    if (expect_hud)
        setenv("D3D12METAL_HUD", "1", 1);
    else
        unsetenv("D3D12METAL_HUD");

    TestContext ctx;
    ComPtr<IDXGIFactory4> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())));
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 320;
    desc.Height = 200;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swap_chain1;
    CHECK_HR(factory->CreateSwapChainForHwnd(ctx.queue.Get(), (HWND)1, &desc, nullptr, nullptr, swap_chain1.ReleaseAndGetAddressOf()));
    ComPtr<IDXGISwapChain3> swap_chain;
    CHECK_HR(swap_chain1.As(&swap_chain));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 2;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    CHECK_HR(ctx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap.ReleaseAndGetAddressOf())));
    const UINT increment = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        const UINT index = swap_chain->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> buffer;
        CHECK_HR(swap_chain->GetBuffer(index, IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(index) * increment;
        ctx.device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        list->ClearRenderTargetView(rtv, black, 0, nullptr);
        CHECK_HR(list->Close());
        ID3D12CommandList *lists[] = {list.Get()};
        ctx.queue->ExecuteCommandLists(1, lists);
        CHECK_HR(swap_chain->Present(0, 0));
        ctx.wait_idle();
    }

    // The dump is written when the command buffer completes: wait for it.
    uint8_t probe[3] = {};
    for (int i = 0; i < 100 && !test_png_pixel(dump.c_str(), 0, 0, probe); ++i)
        usleep(50000);

    // White pixels in the corner where the text goes (scale 2: 12 px margin, 6 x 8 px cells, 3 lines), and a coarse
    // grid over the rest of the frame, which must stay black.
    unsigned white = 0, elsewhere = 0;
    for (size_t y = 0; y < 200; ++y) {
        for (size_t x = 0; x < 320; ++x) {
            const bool corner = x < 160 && y < 80;
            if (!corner && (x % 7 || y % 7))
                continue;
            uint8_t rgb[3] = {};
            CHECK(test_png_pixel(dump.c_str(), x, y, rgb));
            if (corner && rgb[0] > 200 && rgb[1] > 200 && rgb[2] > 200)
                ++white;
            else if (!corner && (rgb[0] || rgb[1] || rgb[2]))
                ++elsewhere;
        }
    }
    std::printf("hud %s: %u white pixels in the corner, %u other non-black pixels\n", expect_hud ? "on" : "off", white, elsewhere);
    if (expect_hud) {
        CHECK(white > 100);
        CHECK(elsewhere == 0);   // shadows are black on black; nothing else is drawn
    } else {
        CHECK(white == 0 && elsewhere == 0);
    }
    return 0;
}
