// Interface versions, pipeline state streams and capability reporting.
#include "color_ps.h"
#include "color_vs.h"
#include "noop_cs.h"
#include "t12.h"

namespace {

template <typename T, typename Source>
bool supports(Source *source)
{
    ComPtr<T> other;
    return SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(other.GetAddressOf())));
}

template <typename T>
T feature(ID3D12Device *device, D3D12_FEATURE which)
{
    T data = {};
    CHECK_HR(device->CheckFeatureSupport(which, &data, sizeof(data)));
    return data;
}

} // namespace

int main()
{
    Gpu ctx;
    ID3D12Device *device = ctx.device.Get();

    // The device answers every interface up to the newest, and the debug layer is absent.
    CHECK((supports<ID3D12Device1>(device)));
    CHECK((supports<ID3D12Device2>(device)));
    CHECK((supports<ID3D12Device3>(device)));
    CHECK((supports<ID3D12Device4>(device)));
    CHECK((supports<ID3D12Device5>(device)));
    CHECK((supports<ID3D12Device6>(device)));
    CHECK((supports<ID3D12Device7>(device)));
    CHECK((supports<ID3D12Device8>(device)));
    CHECK((supports<ID3D12Device9>(device)));
    CHECK((supports<ID3D12Device10>(device)));
    CHECK((supports<ID3D12DeviceRemovedExtendedData>(device)));
    CHECK((supports<ID3D12DeviceRemovedExtendedData2>(device)));
    CHECK(!(supports<ID3D12InfoQueue>(device)));
    CHECK(!(supports<ID3D12DebugDevice>(device)));
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred_settings;
    CHECK_HR(D3D12GetDebugInterface(IID_PPV_ARGS(dred_settings.GetAddressOf())));
    dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    ComPtr<ID3D12Debug> debug;
    CHECK(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf()))));

    ComPtr<ID3D12DeviceRemovedExtendedData2> dred;
    CHECK_HR(device->QueryInterface(IID_PPV_ARGS(dred.GetAddressOf())));
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs;
    CHECK(FAILED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)));
    CHECK(dred->GetDeviceState() == D3D12_DRED_DEVICE_STATE_UNKNOWN);

    ComPtr<ID3D12GraphicsCommandList> list = ctx.list();
    CHECK((supports<ID3D12GraphicsCommandList1>(list.Get())));
    CHECK((supports<ID3D12GraphicsCommandList2>(list.Get())));
    CHECK((supports<ID3D12GraphicsCommandList3>(list.Get())));
    CHECK((supports<ID3D12GraphicsCommandList4>(list.Get())));
    CHECK((supports<ID3D12GraphicsCommandList5>(list.Get())));
    CHECK((supports<ID3D12GraphicsCommandList6>(list.Get())));
    CHECK((supports<ID3D12GraphicsCommandList7>(list.Get())));
    CHECK((supports<ID3D12CommandList>(list.Get())));
    // Unsupported methods log once and do nothing.
    ComPtr<ID3D12GraphicsCommandList6> list6;
    CHECK_HR(list->QueryInterface(IID_PPV_ARGS(list6.GetAddressOf())));
    list6->DispatchMesh(1, 1, 1);
    list6->DispatchMesh(1, 1, 1);
    CHECK_HR(list->Close());

    ComPtr<ID3D12Resource> buffer = ctx.buffer(D3D12_HEAP_TYPE_DEFAULT, 256);
    ComPtr<ID3D12Resource2> buffer2;
    CHECK_HR(buffer.As(&buffer2));
    CHECK(buffer2->GetDesc1().Width == 256);
    CHECK(buffer2->GetDesc1().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
    ComPtr<ID3D12Resource1> buffer1;
    CHECK_HR(buffer.As(&buffer1));

    ComPtr<ID3D12Fence> fence;
    CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.GetAddressOf())));
    ComPtr<ID3D12Fence1> fence1;
    CHECK_HR(fence.As(&fence1));
    CHECK(fence1->GetCreationFlags() == D3D12_FENCE_FLAG_SHARED);

    ComPtr<ID3D12Device4> device4;
    CHECK_HR(device->QueryInterface(IID_PPV_ARGS(device4.GetAddressOf())));
    ComPtr<ID3D12GraphicsCommandList> closed_list;
    CHECK_HR(device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                         IID_PPV_ARGS(closed_list.GetAddressOf())));
    CHECK(closed_list->Close() == E_FAIL);  // created closed

    // CreatePipelineState: a graphics stream.
    const D3D12_ROOT_PARAMETER1 cbv_param = root_descriptor(D3D12_ROOT_PARAMETER_TYPE_CBV, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    ComPtr<ID3D12RootSignature> signature = ctx.root_signature(&cbv_param, 1);
    ComPtr<ID3D12Device2> device2;
    CHECK_HR(device->QueryInterface(IID_PPV_ARGS(device2.GetAddressOf())));
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    struct GraphicsStream {
        StreamObject<ID3D12RootSignature *, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature;
        StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vs;
        StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
        StreamObject<D3D12_INPUT_LAYOUT_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT> input_layout;
        StreamObject<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology;
        StreamObject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> formats;
        StreamObject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
        StreamObject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
        StreamObject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
    } graphics;
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC defaults = graphics_pso_desc(signature.Get(), {}, {}, layout, 1);
    graphics.root_signature.value = signature.Get();
    graphics.vs.value = T12_SHADER(g_color_vs);
    graphics.ps.value = T12_SHADER(g_color_ps);
    graphics.input_layout.value = D3D12_INPUT_LAYOUT_DESC{layout, 1};
    graphics.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.formats.value.NumRenderTargets = 1;
    graphics.formats.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    graphics.rasterizer.value = defaults.RasterizerState;
    graphics.blend.value = defaults.BlendState;
    graphics.sample.value = DXGI_SAMPLE_DESC{1, 0};
    D3D12_PIPELINE_STATE_STREAM_DESC stream = {sizeof(graphics), &graphics};
    ComPtr<ID3D12PipelineState> graphics_pso;
    CHECK_HR(device2->CreatePipelineState(&stream, IID_PPV_ARGS(graphics_pso.GetAddressOf())));

    // A compute stream.
    struct ComputeStream {
        StreamObject<ID3D12RootSignature *, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature;
        StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS> cs;
    } compute;
    compute.root_signature.value = signature.Get();
    compute.cs.value = T12_SHADER(g_noop_cs);
    stream = {sizeof(compute), &compute};
    ComPtr<ID3D12PipelineState> compute_pso;
    CHECK_HR(device2->CreatePipelineState(&stream, IID_PPV_ARGS(compute_pso.GetAddressOf())));
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {};
    compute_desc.pRootSignature = signature.Get();
    compute_desc.CS = T12_SHADER(g_noop_cs);
    ComPtr<ID3D12PipelineState> compute_pso2;
    CHECK_HR(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(compute_pso2.GetAddressOf())));

    // Both stages, an unknown subobject type, a truncated payload and mesh shaders are refused.
    struct Both {
        StreamObject<ID3D12RootSignature *, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature;
        StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vs;
        StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS> cs;
    } both;
    both.root_signature.value = signature.Get();
    both.vs.value = T12_SHADER(g_color_vs);
    both.cs.value = T12_SHADER(g_noop_cs);
    stream = {sizeof(both), &both};
    ComPtr<ID3D12PipelineState> refused;
    CHECK(device2->CreatePipelineState(&stream, IID_PPV_ARGS(refused.GetAddressOf())) == E_INVALIDARG);
    struct Unknown {
        alignas(void *) UINT32 type = 99;
        UINT32 payload = 0;
    } unknown;
    stream = {sizeof(unknown), &unknown};
    CHECK(device2->CreatePipelineState(&stream, IID_PPV_ARGS(refused.GetAddressOf())) == E_INVALIDARG);
    stream = {sizeof(graphics) - 8, &graphics};  // cuts the last subobject short
    CHECK(device2->CreatePipelineState(&stream, IID_PPV_ARGS(refused.GetAddressOf())) == E_INVALIDARG);
    struct Mesh {
        StreamObject<ID3D12RootSignature *, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature;
        StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
    } mesh;
    mesh.root_signature.value = signature.Get();
    mesh.ms.value = T12_SHADER(g_color_vs);
    stream = {sizeof(mesh), &mesh};
    CHECK(device2->CreatePipelineState(&stream, IID_PPV_ARGS(refused.GetAddressOf())) == E_NOTIMPL);

    // Capabilities.
    const auto options = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS>(device, D3D12_FEATURE_D3D12_OPTIONS);
    CHECK(options.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_3);
    CHECK(options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED);
    CHECK(options.TypedUAVLoadAdditionalFormats);
    CHECK(!options.ROVsSupported);
    const auto options1 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS1>(device, D3D12_FEATURE_D3D12_OPTIONS1);
    CHECK(options1.WaveOps && options1.WaveLaneCountMin == 32 && options1.WaveLaneCountMax == 32);
    CHECK(options1.Int64ShaderOps);
    CHECK(feature<D3D12_FEATURE_DATA_D3D12_OPTIONS5>(device, D3D12_FEATURE_D3D12_OPTIONS5).RaytracingTier
          == D3D12_RAYTRACING_TIER_NOT_SUPPORTED);
    CHECK(feature<D3D12_FEATURE_DATA_D3D12_OPTIONS6>(device, D3D12_FEATURE_D3D12_OPTIONS6).VariableShadingRateTier
          == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED);
    const auto options7 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS7>(device, D3D12_FEATURE_D3D12_OPTIONS7);
    CHECK(options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED
          && options7.SamplerFeedbackTier == D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED);
    CHECK(!feature<D3D12_FEATURE_DATA_D3D12_OPTIONS12>(device, D3D12_FEATURE_D3D12_OPTIONS12).EnhancedBarriersSupported);
    for (int which = D3D12_FEATURE_D3D12_OPTIONS8; which <= D3D12_FEATURE_D3D12_OPTIONS18; ++which) {
        if (which == D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT || which == 35)
            continue;
        uint8_t buffer[64] = {};
        const HRESULT hr = device->CheckFeatureSupport(static_cast<D3D12_FEATURE>(which), buffer, 4);
        (void)hr;  // sizes that do not match a structure are refused; no crash is the point
    }
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model = {static_cast<D3D_SHADER_MODEL>(0x69)};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model)));
    CHECK(shader_model.HighestShaderModel == D3D_SHADER_MODEL_6_6);
    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature = {D3D_ROOT_SIGNATURE_VERSION_1_1};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root_signature, sizeof(root_signature)));
    CHECK(root_signature.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_1);
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va = {};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va, sizeof(va)));
    CHECK(va.MaxGPUVirtualAddressBitsPerResource >= 32);

    D3D12_FEATURE_DATA_FORMAT_INFO info = {DXGI_FORMAT_D24_UNORM_S8_UINT, 0};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)));
    CHECK(info.PlaneCount == 2);
    info = {DXGI_FORMAT_R8G8B8A8_UNORM, 0};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)));
    CHECK(info.PlaneCount == 1);

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    CHECK(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
    CHECK(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D);
    CHECK(support.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER);
    CHECK(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    support = {DXGI_FORMAT_D32_FLOAT, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    CHECK(support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL);
    CHECK(!(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET));

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaa = {DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaa, sizeof(msaa)));
    CHECK(msaa.NumQualityLevels == 1);
    msaa = {DXGI_FORMAT_R8G8B8A8_UNORM, 3, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaa, sizeof(msaa)));
    CHECK(msaa.NumQualityLevels == 0);
    msaa = {DXGI_FORMAT_BC1_UNORM, 4, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaa, sizeof(msaa)));
    CHECK(msaa.NumQualityLevels == 0);

    // EnqueueMakeResident signals its fence.
    ComPtr<ID3D12Device3> device3;
    CHECK_HR(device->QueryInterface(IID_PPV_ARGS(device3.GetAddressOf())));
    ID3D12Pageable *pageables[] = {buffer.Get()};
    CHECK_HR(device3->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE, 1, pageables, fence.Get(), 5));
    CHECK(fence->GetCompletedValue() == 5);
    return 0;
}
