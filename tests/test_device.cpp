// SPDX-License-Identifier: LGPL-2.1-or-later
// Device creation, feature queries, and queue/fence synchronisation.
#include "test_context.h"

int main()
{
    ComPtr<IDXGIFactory4> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK_HR(factory->EnumAdapters1(0, adapter.ReleaseAndGetAddressOf()));
    ComPtr<IDXGIAdapter1> no_adapter;
    CHECK(factory->EnumAdapters1(1, no_adapter.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND);

    DXGI_ADAPTER_DESC1 adapter_desc = {};
    CHECK_HR(adapter->GetDesc1(&adapter_desc));
    CHECK(adapter_desc.DedicatedVideoMemory > 0);

    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));
    ComPtr<ID3D12Device2> device2;
    CHECK_HR(device->QueryInterface(IID_PPV_ARGS(device2.ReleaseAndGetAddressOf())));
    CHECK(device->GetNodeCount() == 1);

    // The device runs on the adapter's GPU.
    const LUID luid = device->GetAdapterLuid();
    CHECK(luid.LowPart == adapter_desc.AdapterLuid.LowPart && luid.HighPart == adapter_desc.AdapterLuid.HighPart);
    ComPtr<ID3D12Device> default_device;
    CHECK_HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(default_device.ReleaseAndGetAddressOf())));

    // Feature support.
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));

    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {3, requested, D3D_FEATURE_LEVEL_11_0};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)));
    CHECK(levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_0 || levels.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_11_0);

    D3D12_FEATURE_DATA_ARCHITECTURE architecture = {};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture)));
    CHECK(architecture.UMA);
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture1 = {};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture1, sizeof(architecture1)));
    CHECK(architecture1.UMA && architecture1.IsolatedMMU);
    architecture1.NodeIndex = 1;
    CHECK(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture1, sizeof(architecture1))));

    D3D12_FEATURE_DATA_SHADER_MODEL shader_model = {D3D_SHADER_MODEL_6_7};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model)));
    CHECK(shader_model.HighestShaderModel == D3D_SHADER_MODEL_6_6);

    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature = {D3D_ROOT_SIGNATURE_VERSION_1_1};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root_signature, sizeof(root_signature)));
    CHECK(root_signature.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_1);

    D3D12_FEATURE_DATA_FORMAT_SUPPORT format = {};
    format.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)));
    CHECK(format.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
    format = {};
    format.Format = DXGI_FORMAT_BC7_UNORM;
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)));
    CHECK(format.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D);

    // Queue and fence.
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.ReleaseAndGetAddressOf())));
    ComPtr<ID3D12Pageable> pageable;
    CHECK_HR(queue->QueryInterface(IID_PPV_ARGS(pageable.ReleaseAndGetAddressOf())));

    ComPtr<ID3D12Fence> fence;
    CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf())));
    CHECK(fence->GetCompletedValue() == 0);

    CHECK_HR(queue->Signal(fence.Get(), 5));
    CHECK_HR(fence->SetEventOnCompletion(5, nullptr));
    CHECK(fence->GetCompletedValue() == 5);

    // CPU signal, then a GPU wait followed by a GPU signal.
    CHECK_HR(fence->Signal(7));
    CHECK(fence->GetCompletedValue() == 7);
    CHECK_HR(queue->Wait(fence.Get(), 7));
    CHECK_HR(queue->Signal(fence.Get(), 9));
    CHECK_HR(fence->SetEventOnCompletion(9, nullptr));
    CHECK(fence->GetCompletedValue() == 9);

    return 0;
}
