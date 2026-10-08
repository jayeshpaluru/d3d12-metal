#include "d3d12/pipeline_state.h"

#include <atomic>
#include <cstring>

#include "common/log.h"
#include "common/stats.h"
#include "d3d12/device.h"
#include "d3d12/formats.h"
#include "d3d12/root_signature.h"

namespace d3d12m {

namespace {

// The mtlb enums share their numeric values with the D3D12 enums they mirror.
mtlb_stencil_face convert(const D3D12_DEPTH_STENCILOP_DESC &face)
{
    return {face.StencilFailOp, face.StencilDepthFailOp, face.StencilPassOp, face.StencilFunc};
}

mtlb_render_target_blend convert(const D3D12_RENDER_TARGET_BLEND_DESC &rt)
{
    return {static_cast<uint32_t>(rt.BlendEnable), rt.SrcBlend, rt.DestBlend, rt.BlendOp,
            rt.SrcBlendAlpha, rt.DestBlendAlpha, rt.BlendOpAlpha, rt.RenderTargetWriteMask};
}

static_assert(int(MTLB_TOPOLOGY_TYPE_PATCH) == int(D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH), "mtlb topology types mirror the D3D12 enum");
static_assert(int(MTLB_TOPOLOGY_TYPE_POINT) == int(D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT)
                  && int(MTLB_TOPOLOGY_TYPE_LINE) == int(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE)
                  && int(MTLB_TOPOLOGY_TYPE_TRIANGLE) == int(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE),
              "mtlb topology types mirror the D3D12 enum");

// FNV-1a over a byte range, continuing from `hash`.
uint64_t hash_bytes(const void *data, size_t size, uint64_t hash = 14695981039346656037ull)
{
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ static_cast<const uint8_t *>(data)[i]) * 1099511628211ull;
    return hash;
}

// Identifies a pipeline description for the failure cache: the state, the root signature and the shader contents (not
// their addresses or handles: those are reused by later objects).
uint64_t root_signature_hash(const RootSignature &signature)
{
    const RootSignatureKey &key = signature.content_key();
    return hash_bytes(&key, sizeof(key));
}

uint64_t pipeline_key(mtlb_pipeline_desc pd, const RootSignature &signature)
{
    const uint64_t vs = hash_bytes(pd.vs_dxil, pd.vs_size);
    const uint64_t ps = pd.ps_dxil ? hash_bytes(pd.ps_dxil, pd.ps_size) : 0;
    const uint64_t gs = pd.gs_dxil ? hash_bytes(pd.gs_dxil, pd.gs_size) : 0;
    const uint64_t hs = pd.hs_dxil ? hash_bytes(pd.hs_dxil, pd.hs_size) : 0;
    const uint64_t ds = pd.ds_dxil ? hash_bytes(pd.ds_dxil, pd.ds_size) : 0;
    pd.vs_dxil = pd.ps_dxil = pd.gs_dxil = pd.hs_dxil = pd.ds_dxil = nullptr;
    pd.vs_entry = pd.ps_entry = nullptr;
    pd.root_signature = 0;
    const uint64_t stages[6] = {vs, ps, gs, hs, ds, root_signature_hash(signature)};
    return hash_bytes(&pd, sizeof(pd), hash_bytes(stages, sizeof(stages)));
}

uint64_t pipeline_key(mtlb_compute_pipeline_desc pd, const RootSignature &signature)
{
    const uint64_t cs = hash_bytes(pd.cs_dxil, pd.cs_size);
    pd.cs_dxil = nullptr;
    pd.cs_entry = nullptr;
    pd.root_signature = 0;
    return hash_bytes(&pd, sizeof(pd), cs ^ root_signature_hash(signature)) ^ 0x9e3779b97f4a7c15ull;
}

// Failures that say something about the description (a shader that does not convert, a state Metal refuses) are
// remembered; those that come from the moment (memory, a device that went away) are retried.
bool is_permanent_failure(mtlb_result result)
{
    return result == MTLB_ERROR_COMPILE_FAILED || result == MTLB_ERROR_UNSUPPORTED || result == MTLB_ERROR_INVALID_ARGUMENT;
}

// A pipeline created without a root signature takes the one embedded in its shaders (the RTS0 part of the DXIL
// container): XeSS and some engines compile their shaders that way. One object per distinct signature serves all pipelines
// (Device::find_embedded_root_signature), the pipeline keeps its own reference.
struct EmbeddedRootSignature {
    RootSignature *signature = nullptr;
    ~EmbeddedRootSignature()
    {
        if (signature)
            signature->release_internal_ref();
    }
    // The signature of the first of `shaders` that carries one.
    RootSignature *from(Device *device, std::initializer_list<D3D12_SHADER_BYTECODE> shaders)
    {
        for (const D3D12_SHADER_BYTECODE &shader : shaders) {
            if (shader.pShaderBytecode && shader.BytecodeLength
                && SUCCEEDED(RootSignature::acquire_embedded(device, shader.pShaderBytecode, shader.BytecodeLength, &signature)))
                return signature;
        }
        return nullptr;
    }
};

} // namespace

HRESULT PipelineState::create_graphics(Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
                                       REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!desc.VS.pShaderBytecode || !desc.VS.BytecodeLength
        || desc.NumRenderTargets > MTLB_MAX_RENDER_TARGETS || desc.InputLayout.NumElements > MTLB_MAX_INPUT_ELEMENTS)
        return E_INVALIDARG;
    if (desc.StreamOutput.NumEntries) {
        // Metal Shader Converter has no public stream-output support; games that need it get this refusal.
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1) < 8)
            D3D12M_LOG("stream output is not supported (%u entries)", desc.StreamOutput.NumEntries);
        return E_NOTIMPL;
    }
    if ((desc.HS.BytecodeLength != 0) != (desc.DS.BytecodeLength != 0))
        return E_INVALIDARG;

    // Only RootSignature objects of this layer can be passed in.
    EmbeddedRootSignature embedded;
    RootSignature *root_signature = desc.pRootSignature ? ours<RootSignature>(desc.pRootSignature) : embedded.from(device, {desc.VS, desc.PS, desc.GS, desc.HS, desc.DS});
    if (!root_signature)
        return E_INVALIDARG;

    mtlb_pipeline_desc pd{};
    pd.vs_dxil = desc.VS.pShaderBytecode;
    pd.vs_size = desc.VS.BytecodeLength;
    pd.ps_dxil = desc.PS.pShaderBytecode;
    pd.ps_size = desc.PS.BytecodeLength;
    pd.root_signature = root_signature->handle();
    if (desc.GS.BytecodeLength && desc.GS.pShaderBytecode) {
        pd.gs_dxil = desc.GS.pShaderBytecode;
        pd.gs_size = desc.GS.BytecodeLength;
    }
    if (desc.HS.BytecodeLength && desc.HS.pShaderBytecode && desc.DS.BytecodeLength && desc.DS.pShaderBytecode) {
        pd.hs_dxil = desc.HS.pShaderBytecode;
        pd.hs_size = desc.HS.BytecodeLength;
        pd.ds_dxil = desc.DS.pShaderBytecode;
        pd.ds_size = desc.DS.BytecodeLength;
    }

    pd.num_render_targets = desc.NumRenderTargets;
    for (UINT i = 0; i < desc.NumRenderTargets; ++i) {
        pd.rtv_formats[i] = to_mtlb_format(desc.RTVFormats[i]);
        if (pd.rtv_formats[i] == MTLB_FORMAT_UNKNOWN && desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN)
            return E_INVALIDARG;
    }
    pd.dsv_format = to_mtlb_format(desc.DSVFormat);
    if (pd.dsv_format == MTLB_FORMAT_UNKNOWN && desc.DSVFormat != DXGI_FORMAT_UNKNOWN)
        return E_INVALIDARG;
    pd.sample_count = desc.SampleDesc.Count;
    pd.topology_type = desc.PrimitiveTopologyType;

    pd.independent_blend = desc.BlendState.IndependentBlendEnable;
    for (UINT i = 0; i < MTLB_MAX_RENDER_TARGETS; ++i)
        pd.blend[i] = convert(desc.BlendState.RenderTarget[i]);

    const D3D12_RASTERIZER_DESC &r = desc.RasterizerState;
    pd.cull_mode = r.CullMode;
    pd.front_counter_clockwise = r.FrontCounterClockwise;
    pd.fill_mode = r.FillMode;
    pd.depth_bias = r.DepthBias;
    pd.depth_bias_clamp = r.DepthBiasClamp;
    pd.slope_scaled_depth_bias = r.SlopeScaledDepthBias;
    pd.depth_clip_enable = r.DepthClipEnable;

    const D3D12_DEPTH_STENCIL_DESC &ds = desc.DepthStencilState;
    pd.depth_enable = ds.DepthEnable;
    pd.depth_write_enable = ds.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
    pd.depth_func = ds.DepthFunc;
    pd.stencil_enable = ds.StencilEnable;
    pd.stencil_read_mask = ds.StencilReadMask;
    pd.stencil_write_mask = ds.StencilWriteMask;
    pd.front_face = convert(ds.FrontFace);
    pd.back_face = convert(ds.BackFace);

    pd.num_input_elements = desc.InputLayout.NumElements;
    UINT next_offset[MTLB_MAX_VERTEX_BUFFERS] = {};  // for D3D12_APPEND_ALIGNED_ELEMENT
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC &e = desc.InputLayout.pInputElementDescs[i];
        mtlb_format_info info;
        if (e.InputSlot >= MTLB_MAX_VERTEX_BUFFERS || !get_format_info(e.Format, &info))
            return E_INVALIDARG;
        mtlb_input_element &m = pd.input_elements[i];
        std::strncpy(m.semantic_name, e.SemanticName, sizeof(m.semantic_name) - 1);
        m.semantic_index = e.SemanticIndex;
        m.format = to_mtlb_format(e.Format);
        m.input_slot = e.InputSlot;
        m.byte_offset = e.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? next_offset[e.InputSlot]
                                                                            : e.AlignedByteOffset;
        next_offset[e.InputSlot] = m.byte_offset + info.bytes_per_block;
        m.input_class = e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                            ? MTLB_INPUT_PER_INSTANCE : MTLB_INPUT_PER_VERTEX;
        m.step_rate = e.InstanceDataStepRate;
    }

    const uint64_t key = pipeline_key(pd, *root_signature);
    HRESULT known_failure;
    if (device->failed_pipeline(key, &known_failure))
        return known_failure;
    stat_add(Stat::PsoCreations);
    PsoTimer timer;
    auto *pso = new PipelineState(device);
    mtlb_result result = mtlb_pipeline_create(device->handle(), &pd, &pso->pipeline_);
    if (result != MTLB_OK) {
        if (is_permanent_failure(result) && device->note_failed_pipeline(key, to_hresult(result)))
            D3D12M_LOG("pipeline creation failed (not retried): %s", mtlb_last_error());
        pso->Release();
        return to_hresult(result);
    }
    pso->root_signature_ = root_signature;
    root_signature->AddRef();

    return hand_out(pso, riid, out);
}

HRESULT PipelineState::create_compute(Device *device, const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, REFIID riid,
                                      void **out)
{
    if (!out)
        return E_POINTER;
    EmbeddedRootSignature embedded;
    RootSignature *root_signature = desc.pRootSignature ? ours<RootSignature>(desc.pRootSignature) : embedded.from(device, {desc.CS});
    if (!root_signature || !desc.CS.pShaderBytecode || !desc.CS.BytecodeLength) {
        D3D12M_LOG("compute pipeline: root signature %p (ours: %d), CS %p size %zu", static_cast<void *>(desc.pRootSignature),
                   root_signature != nullptr, desc.CS.pShaderBytecode, desc.CS.BytecodeLength);
        return E_INVALIDARG;
    }

    mtlb_compute_pipeline_desc pd{};
    pd.cs_dxil = desc.CS.pShaderBytecode;
    pd.cs_size = desc.CS.BytecodeLength;
    pd.root_signature = root_signature->handle();

    const uint64_t key = pipeline_key(pd, *root_signature);
    HRESULT known_failure;
    if (device->failed_pipeline(key, &known_failure))
        return known_failure;
    stat_add(Stat::PsoCreations);
    PsoTimer timer;
    auto *pso = new PipelineState(device);
    mtlb_result result = mtlb_compute_pipeline_create(device->handle(), &pd, &pso->pipeline_);
    if (result != MTLB_OK) {
        if (is_permanent_failure(result) && device->note_failed_pipeline(key, to_hresult(result)))
            D3D12M_LOG("compute pipeline creation failed (not retried): %s", mtlb_last_error());
        pso->Release();
        return to_hresult(result);
    }
    pso->compute_ = true;
    pso->root_signature_ = root_signature;
    root_signature->AddRef();
    return hand_out(pso, riid, out);
}

PipelineState::~PipelineState()
{
    if (pipeline_)
        mtlb_pipeline_destroy(pipeline_);
    safe_release(root_signature_);
}

} // namespace d3d12m
