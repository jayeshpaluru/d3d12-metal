// SPDX-License-Identifier: LGPL-2.1-or-later
// The window-less scenarios of the native test suite, run under Wine: they prove
// the PE front-end, the unix-call transport and the Metal backend end to end.
// Device and adapter, buffer copies, an offscreen triangle, fence events.
#include <cstring>
#include <vector>

#include "color_ps.h"
#include "color_vs.h"
#include "wine_test.h"

namespace {

void scenario_device()
{
    DeviceContext ctx;
    ComPtr<IDXGIAdapter1> adapter;
    CHECK_HR(ctx.factory->EnumAdapters1(0, &adapter));
    DXGI_ADAPTER_DESC1 desc;
    CHECK_HR(adapter->GetDesc1(&desc));
    char name[128] = {};
    for (int i = 0; i < 127 && desc.Description[i]; ++i)
        name[i] = static_cast<char>(desc.Description[i]);
    std::printf("  adapter: %s\n", name);
    CHECK(desc.VendorId == 0x106B);

    D3D12_COMMAND_QUEUE_DESC queue_desc = ctx.queue->GetDesc();
    CHECK(queue_desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT);
    LUID luid = ctx.device->GetAdapterLuid();
    CHECK(luid.LowPart == desc.AdapterLuid.LowPart && luid.HighPart == desc.AdapterLuid.HighPart);

    D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_12_0};
    levels.NumFeatureLevels = 2;
    levels.pFeatureLevelsRequested = requested;
    CHECK_HR(ctx.device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)));
    CHECK(levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_11_0);

    // A swap-chain-less factory still answers the newer interfaces.
    ComPtr<IDXGIFactory5> factory5;
    CHECK_HR(ctx.factory.As(&factory5));
}

void scenario_copy()
{
    DeviceContext ctx;
    constexpr UINT64 kSize = 4096;
    ComPtr<ID3D12Resource> gpu = ctx.create_buffer(D3D12_HEAP_TYPE_DEFAULT, kSize);
    ComPtr<ID3D12Resource> readback = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, kSize);
    CHECK(gpu->GetDesc().Width == kSize);

    std::vector<unsigned char> pattern(kSize);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<unsigned char>(i * 7 + 3);
    ComPtr<ID3D12Resource> upload = ctx.create_upload_buffer(pattern.data(), kSize);

    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK_HR(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK_HR(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&list)));
    list->CopyBufferRegion(gpu.Get(), 0, upload.Get(), 0, kSize);
    list->CopyResource(readback.Get(), gpu.Get());
    CHECK_HR(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    ctx.queue->ExecuteCommandLists(1, lists);
    ctx.wait_idle();

    void *mapped = nullptr;
    CHECK_HR(readback->Map(0, nullptr, &mapped));
    CHECK(std::memcmp(mapped, pattern.data(), kSize) == 0);
    readback->Unmap(0, nullptr);
}

void scenario_triangle()
{
    constexpr UINT kSize = 64;
    DeviceContext ctx;

    const D3D12_HEAP_PROPERTIES default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC target_desc =
        texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ComPtr<ID3D12Resource> target;
    CHECK_HR(ctx.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    CHECK_HR(ctx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    CHECK(rtv.ptr != 0);
    ctx.device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    // Four root constants (the colour) at b0 for the pixel shader.
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 4;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ComPtr<ID3D12RootSignature> signature = ctx.create_root_signature(&param, 1);

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc =
        default_pipeline_desc(signature.Get(), layout, 1, g_color_vs, sizeof(g_color_vs), g_color_ps,
                              sizeof(g_color_ps), DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D12PipelineState> pso;
    CHECK_HR(ctx.device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

    const float vertices[] = {-0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f};
    ComPtr<ID3D12Resource> vertex_buffer = ctx.create_upload_buffer(vertices, sizeof(vertices));
    D3D12_VERTEX_BUFFER_VIEW vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 3 * sizeof(float)};

    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK_HR(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK_HR(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                           IID_PPV_ARGS(&list)));

    const float clear_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const float triangle_color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
    D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
    list->SetGraphicsRootSignature(signature.Get());
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &vbv);
    list->SetGraphicsRoot32BitConstants(0, 4, triangle_color, 0);
    list->DrawInstanced(3, 1, 0, 0);

    const D3D12_RESOURCE_BARRIER barrier =
        transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
    D3D12_RESOURCE_DESC desc = target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    ctx.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> readback = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, total);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = target.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK_HR(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    ctx.queue->ExecuteCommandLists(1, lists);
    ctx.wait_idle();

    unsigned char *mapped = nullptr;
    CHECK_HR(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
    auto pixel = [&](UINT x, UINT y) {
        Pixel p;
        std::memcpy(&p, mapped + size_t(y) * footprint.Footprint.RowPitch + x * sizeof(Pixel), sizeof(p));
        return p;
    };
    const Pixel center = pixel(32, 32), corner = pixel(0, 0);
    readback->Unmap(0, nullptr);
    std::printf("  center (%u,%u,%u,%u) corner (%u,%u,%u,%u)\n", center.r, center.g, center.b, center.a, corner.r,
                corner.g, corner.b, corner.a);
    CHECK(near_pixel(center, {255, 0, 0, 255}));
    CHECK(near_pixel(corner, {0, 0, 255, 255}));
}

DWORD WINAPI signal_after_delay(void *param)
{
    Sleep(100);
    return static_cast<ID3D12Fence *>(param)->Signal(7) == S_OK ? 0 : 1;
}

void scenario_fence_events()
{
    DeviceContext ctx;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(event);

    // Already reached: signaled before the call returns.
    CHECK_HR(ctx.fence->Signal(1));
    CHECK_HR(ctx.fence->SetEventOnCompletion(1, event));
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);

    // Not reached yet: stays unsignaled until the queue signals the fence.
    CHECK_HR(ctx.fence->SetEventOnCompletion(10, event));
    CHECK(WaitForSingleObject(event, 50) == WAIT_TIMEOUT);
    CHECK_HR(ctx.queue->Signal(ctx.fence.Get(), 10));
    CHECK(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
    CHECK(ctx.fence->GetCompletedValue() >= 10);

    // Signaled from another Windows thread.
    ComPtr<ID3D12Fence> other;
    CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&other)));
    CHECK_HR(other->SetEventOnCompletion(7, event));
    HANDLE thread = CreateThread(nullptr, 0, signal_after_delay, other.Get(), 0, nullptr);
    CHECK(thread);
    CHECK(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    DWORD exit_code = 1;
    GetExitCodeThread(thread, &exit_code);
    CHECK(exit_code == 0);
    CloseHandle(thread);

    // A fence released with a pending wait must not hang or crash.
    ComPtr<ID3D12Fence> pending;
    CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pending)));
    CHECK_HR(pending->SetEventOnCompletion(100, event));
    pending.Reset();

    CloseHandle(event);
}

struct Scenario {
    const char *name;
    void (*run)();
};

} // namespace

int main()
{
    const Scenario scenarios[] = {
        {"device", scenario_device},
        {"copy", scenario_copy},
        {"triangle", scenario_triangle},
        {"fence events", scenario_fence_events},
    };
    for (const Scenario &scenario : scenarios) {
        std::printf("wine_basic: %s\n", scenario.name);
        std::fflush(stdout);
        scenario.run();
        std::printf("wine_basic: %s PASS\n", scenario.name);
        std::fflush(stdout);
    }
    std::printf("wine_basic: PASS\n");
    return 0;
}
