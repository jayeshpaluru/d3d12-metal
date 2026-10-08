// The feature level the layer reports: 12_0 by default; `feature_level=12_1` (D3D12METAL_FEATURE_LEVEL=12_1) raises it to
// 12_1 and reports rasterizer ordered views, which the converter backs (tests/portable/p_rov.cpp), while conservative
// rasterization stays unsupported. Run as `test_feature_level 12_0` and `test_feature_level 12_1` with the variable set.
#include <cstring>

#include "portable/t12.h"

int main(int argc, char **argv)
{
    CHECK(argc > 1);
    const bool level_12_1 = std::strcmp(argv[1], "12_1") == 0;

    ComPtr<IDXGIFactory4> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK_HR(factory->EnumAdapters1(0, adapter.GetAddressOf()));

    // A device at the level the game asks for exists only up to the layer's maximum.
    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.GetAddressOf())));
    ComPtr<ID3D12Device> other;
    const HRESULT at_12_1 = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(other.GetAddressOf()));
    CHECK_EQ(SUCCEEDED(at_12_1), level_12_1);
    CHECK(FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(other.GetAddressOf()))));

    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                           D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
    levels.NumFeatureLevels = 5;
    levels.pFeatureLevelsRequested = requested;
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)));
    CHECK_EQ(levels.MaxSupportedFeatureLevel, level_12_1 ? D3D_FEATURE_LEVEL_12_1 : D3D_FEATURE_LEVEL_12_0);

    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    CHECK_EQ(options.ROVsSupported, level_12_1);
    CHECK_EQ(options.ConservativeRasterizationTier, D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED);

    std::printf("test_feature_level: ok (%s)\n", level_12_1 ? "12_1" : "12_0");
    return 0;
}
