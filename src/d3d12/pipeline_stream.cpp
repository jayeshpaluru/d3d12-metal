// CreatePipelineState: parsing of the pipeline state stream.
//
// The stream is a sequence of subobjects, each starting at a pointer-aligned
// offset with a 32-bit type, followed by its payload at the payload's own
// alignment. Unknown types make the whole stream invalid, as in the D3D12
// runtime. The result feeds the same creation paths as the descriptor structs.
#include <cstring>

#include "d3d12/device.h"
#include "d3d12/formats.h"
#include "d3d12/pipeline_state.h"

namespace d3d12m {

namespace {

// Subobject type values (D3D12_PIPELINE_STATE_SUBOBJECT_TYPE). Spelled out because
// the MinGW headers lack the newest enumerators.
enum Subobject : uint32_t {
    kRootSignature = 0, kVS, kPS, kDS, kHS, kGS, kCS, kStreamOutput, kBlend, kSampleMask, kRasterizer, kDepthStencil,
    kInputLayout, kIBStripCutValue, kPrimitiveTopology, kRenderTargetFormats, kDepthStencilFormat, kSampleDesc,
    kNodeMask, kCachedPSO, kFlags, kDepthStencil1, kViewInstancing, kGenericProgram,
    kAS, kMS, kDepthStencil2, kRasterizer1, kRasterizer2, kSerializedRootSignature,
};

// Payloads newer than the oldest header this builds against.
struct DepthStencilOp1 {
    D3D12_STENCIL_OP StencilFailOp, StencilDepthFailOp, StencilPassOp;
    D3D12_COMPARISON_FUNC StencilFunc;
    UINT8 StencilReadMask, StencilWriteMask;
};
struct DepthStencil2 {
    BOOL DepthEnable;
    D3D12_DEPTH_WRITE_MASK DepthWriteMask;
    D3D12_COMPARISON_FUNC DepthFunc;
    BOOL StencilEnable;
    DepthStencilOp1 FrontFace, BackFace;
    BOOL DepthBoundsTestEnable;
};
struct Rasterizer1 {
    D3D12_FILL_MODE FillMode;
    D3D12_CULL_MODE CullMode;
    BOOL FrontCounterClockwise;
    FLOAT DepthBias, DepthBiasClamp, SlopeScaledDepthBias;
    BOOL DepthClipEnable, MultisampleEnable, AntialiasedLineEnable;
    UINT ForcedSampleCount;
    D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
};
struct Rasterizer2 {
    D3D12_FILL_MODE FillMode;
    D3D12_CULL_MODE CullMode;
    BOOL FrontCounterClockwise;
    FLOAT DepthBias, DepthBiasClamp, SlopeScaledDepthBias;
    BOOL DepthClipEnable;
    UINT LineRasterizationMode;
    UINT ForcedSampleCount;
    D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
};
struct SerializedRootSignature {
    const void *data;
    SIZE_T size;
};
struct RtFormats {
    DXGI_FORMAT RTFormats[8];
    UINT NumRenderTargets;
};

struct Parsed {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    bool has_graphics_stage = false;
    bool has_compute_stage = false;
    bool mesh_pipeline = false;
    SerializedRootSignature serialized{};
};

// Reads the payload `T` of a subobject.
template <typename T>
bool read(const uint8_t *data, size_t size, size_t &offset, T &out)
{
    const size_t at = align_up(offset + sizeof(uint32_t), alignof(T));
    if (size < sizeof(T) || at > size - sizeof(T))
        return false;
    std::memcpy(&out, data + at, sizeof(T));
    offset = align_up(at + sizeof(T), sizeof(void *));
    return true;
}

D3D12_DEPTH_STENCILOP_DESC convert(const DepthStencilOp1 &op, UINT8 *read_mask, UINT8 *write_mask)
{
    // The per-face masks of DESC2 collapse to the front face's: Metal has one pair.
    *read_mask = op.StencilReadMask;
    *write_mask = op.StencilWriteMask;
    return {op.StencilFailOp, op.StencilDepthFailOp, op.StencilPassOp, op.StencilFunc};
}

HRESULT parse(const D3D12_PIPELINE_STATE_STREAM_DESC &stream, Parsed &out)
{
    const auto *data = static_cast<const uint8_t *>(stream.pPipelineStateSubobjectStream);
    const size_t size = stream.SizeInBytes;
    if (!data && size)
        return E_INVALIDARG;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC &g = out.graphics;
    g.SampleMask = UINT_MAX;
    g.SampleDesc.Count = 1;

    size_t offset = 0;
    while (offset < size) {
        if (size - offset < sizeof(uint32_t))
            return E_INVALIDARG;
        uint32_t type;
        std::memcpy(&type, data + offset, sizeof(type));
        bool ok = true;
        switch (type) {
        case kRootSignature: {
            ID3D12RootSignature *rs;
            ok = read(data, size, offset, rs);
            g.pRootSignature = rs;
            out.compute.pRootSignature = rs;
            break;
        }
        case kSerializedRootSignature:
            ok = read(data, size, offset, out.serialized);
            break;
        case kVS: ok = read(data, size, offset, g.VS); out.has_graphics_stage = true; break;
        case kPS: ok = read(data, size, offset, g.PS); out.has_graphics_stage = true; break;
        case kDS: ok = read(data, size, offset, g.DS); out.has_graphics_stage = true; break;
        case kHS: ok = read(data, size, offset, g.HS); out.has_graphics_stage = true; break;
        case kGS: ok = read(data, size, offset, g.GS); out.has_graphics_stage = true; break;
        case kCS: ok = read(data, size, offset, out.compute.CS); out.has_compute_stage = true; break;
        case kAS:
        case kMS: {
            D3D12_SHADER_BYTECODE ignored;
            ok = read(data, size, offset, ignored);
            out.mesh_pipeline = true;
            break;
        }
        case kStreamOutput: ok = read(data, size, offset, g.StreamOutput); break;
        case kBlend: ok = read(data, size, offset, g.BlendState); break;
        case kSampleMask: ok = read(data, size, offset, g.SampleMask); break;
        case kRasterizer: ok = read(data, size, offset, g.RasterizerState); break;
        case kDepthStencil: ok = read(data, size, offset, g.DepthStencilState); break;
        case kInputLayout: ok = read(data, size, offset, g.InputLayout); break;
        case kIBStripCutValue: ok = read(data, size, offset, g.IBStripCutValue); break;
        case kPrimitiveTopology: ok = read(data, size, offset, g.PrimitiveTopologyType); break;
        case kRenderTargetFormats: {
            RtFormats formats;
            ok = read(data, size, offset, formats);
            if (ok) {
                if (formats.NumRenderTargets > 8)
                    return E_INVALIDARG;
                g.NumRenderTargets = formats.NumRenderTargets;
                std::copy_n(formats.RTFormats, 8, g.RTVFormats);
            }
            break;
        }
        case kDepthStencilFormat: ok = read(data, size, offset, g.DSVFormat); break;
        case kSampleDesc: ok = read(data, size, offset, g.SampleDesc); break;
        case kNodeMask: ok = read(data, size, offset, g.NodeMask); break;
        case kCachedPSO: ok = read(data, size, offset, g.CachedPSO); break;
        case kFlags: ok = read(data, size, offset, g.Flags); break;
        case kViewInstancing: {
            D3D12_VIEW_INSTANCING_DESC view_instancing;
            ok = read(data, size, offset, view_instancing);
            if (ok && view_instancing.ViewInstanceCount > 1)
                D3D12M_LOG("view instancing is not supported, only the first view is rendered");
            break;
        }
        case kDepthStencil1: {
            D3D12_DEPTH_STENCIL_DESC1 ds;
            ok = read(data, size, offset, ds);
            if (ok) {
                D3D12_DEPTH_STENCIL_DESC &d = g.DepthStencilState;
                d = {ds.DepthEnable, ds.DepthWriteMask, ds.DepthFunc, ds.StencilEnable, ds.StencilReadMask,
                     ds.StencilWriteMask, ds.FrontFace, ds.BackFace};
                // DepthBoundsTestEnable: Metal has no depth bounds test; ignored.
            }
            break;
        }
        case kDepthStencil2: {
            DepthStencil2 ds;
            ok = read(data, size, offset, ds);
            if (ok) {
                D3D12_DEPTH_STENCIL_DESC &d = g.DepthStencilState;
                UINT8 back_read, back_write;
                d.DepthEnable = ds.DepthEnable;
                d.DepthWriteMask = ds.DepthWriteMask;
                d.DepthFunc = ds.DepthFunc;
                d.StencilEnable = ds.StencilEnable;
                d.FrontFace = convert(ds.FrontFace, &d.StencilReadMask, &d.StencilWriteMask);
                d.BackFace = convert(ds.BackFace, &back_read, &back_write);
            }
            break;
        }
        case kRasterizer1: {
            Rasterizer1 r;
            ok = read(data, size, offset, r);
            if (ok) {
                D3D12_RASTERIZER_DESC &d = g.RasterizerState;
                d = {r.FillMode, r.CullMode, r.FrontCounterClockwise, static_cast<INT>(r.DepthBias), r.DepthBiasClamp,
                     r.SlopeScaledDepthBias, r.DepthClipEnable, r.MultisampleEnable, r.AntialiasedLineEnable,
                     r.ForcedSampleCount, r.ConservativeRaster};
            }
            break;
        }
        case kRasterizer2: {
            Rasterizer2 r;
            ok = read(data, size, offset, r);
            if (ok) {
                D3D12_RASTERIZER_DESC &d = g.RasterizerState;
                d = {r.FillMode, r.CullMode, r.FrontCounterClockwise, static_cast<INT>(r.DepthBias), r.DepthBiasClamp,
                     r.SlopeScaledDepthBias, r.DepthClipEnable, FALSE, FALSE, r.ForcedSampleCount, r.ConservativeRaster};
            }
            break;
        }
        default:
            D3D12M_LOG("CreatePipelineState: unknown subobject type %u", type);
            return E_INVALIDARG;
        }
        if (!ok)
            return E_INVALIDARG;
    }
    out.compute.NodeMask = g.NodeMask;
    return S_OK;
}

} // namespace

HRESULT PipelineState::create_from_stream(Device *device, const D3D12_PIPELINE_STATE_STREAM_DESC &stream, REFIID riid,
                                          void **out)
{
    if (!out)
        return E_POINTER;
    Parsed parsed;
    HRESULT hr = parse(stream, parsed);
    if (FAILED(hr))
        return hr;
    if (parsed.mesh_pipeline) {
        D3D12M_LOG("mesh and amplification shaders are not supported");
        return E_NOTIMPL;
    }
    if (parsed.has_compute_stage == parsed.has_graphics_stage)
        return E_INVALIDARG;

    // A serialized root signature stands in for a root signature object.
    RootSignature *temporary = nullptr;
    if (!parsed.graphics.pRootSignature && parsed.serialized.data) {
        ID3D12RootSignature *created = nullptr;
        hr = RootSignature::create(device, parsed.serialized.data, parsed.serialized.size,
                                   __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&created));
        if (FAILED(hr))
            return hr;
        temporary = ours<RootSignature>(created);
        parsed.graphics.pRootSignature = parsed.compute.pRootSignature = created;
    }
    hr = parsed.has_compute_stage ? create_compute(device, parsed.compute, riid, out)
                                  : create_graphics(device, parsed.graphics, riid, out);
    if (temporary)
        temporary->Release();  // the pipeline holds its own reference
    return hr;
}

} // namespace d3d12m
