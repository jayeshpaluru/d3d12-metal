#include "d3d12/command_signature.h"

#include "d3d12/device.h"

namespace d3d12m {

HRESULT CommandSignature::create(Device *device, const D3D12_COMMAND_SIGNATURE_DESC &desc, ID3D12RootSignature *root_signature,
                                 REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (desc.NumArgumentDescs == 0 || !desc.pArgumentDescs || desc.ByteStride == 0)
        return E_INVALIDARG;

    UINT argument_bytes = 0;
    bool needs_root_signature = false;
    for (UINT i = 0; i < desc.NumArgumentDescs; ++i) {
        const D3D12_INDIRECT_ARGUMENT_DESC &a = desc.pArgumentDescs[i];
        const bool last = i + 1 == desc.NumArgumentDescs;
        switch (a.Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
            if (!last)
                return E_INVALIDARG;  // the action ends the command
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            if (a.VertexBuffer.Slot >= MTLB_MAX_VERTEX_BUFFERS)
                return E_INVALIDARG;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
            needs_root_signature = true;
            break;
        default:
            // An index buffer view is a parameter of the draw call in Metal, which the GPU cannot change;
            // ray tracing and mesh shading are not supported.
            D3D12M_LOG("CreateCommandSignature: argument type %d is not supported", static_cast<int>(a.Type));
            return E_NOTIMPL;
        }
        argument_bytes += indirect_argument_size(a);
    }
    const D3D12_INDIRECT_ARGUMENT_TYPE action = desc.pArgumentDescs[desc.NumArgumentDescs - 1].Type;
    if (action != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW && action != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
        && action != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)
        return E_INVALIDARG;
    if (argument_bytes > desc.ByteStride)
        return E_INVALIDARG;
    auto *rs = ours<RootSignature>(root_signature);
    if (needs_root_signature && !rs)
        return E_INVALIDARG;

    auto *signature = new CommandSignature(device);
    signature->stride_ = desc.ByteStride;
    signature->argument_bytes_ = argument_bytes;
    try {
        signature->arguments_.assign(desc.pArgumentDescs, desc.pArgumentDescs + desc.NumArgumentDescs);
    } catch (const std::bad_alloc &) {
        signature->Release();
        return E_OUTOFMEMORY;
    }
    if (rs) {
        rs->AddRef();
        signature->root_signature_ = rs;
    }
    return hand_out(signature, riid, out);
}

} // namespace d3d12m
