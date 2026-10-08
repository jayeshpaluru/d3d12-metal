// ID3D12CommandSignature: the layout of the commands ExecuteIndirect reads.
#pragma once

#include <vector>

#include "d3d12/object.h"
#include "d3d12/root_signature.h"

namespace d3d12m {

class CommandSignature final : public ChildImpl<ID3D12CommandSignature> {
public:
    // E_NOTIMPL for signatures with arguments that cannot be translated (index buffer views, ray tracing, mesh shading).
    static HRESULT create(Device *device, const D3D12_COMMAND_SIGNATURE_DESC &desc, ID3D12RootSignature *root_signature,
                          REFIID riid, void **out);

    UINT stride() const { return stride_; }
    const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &arguments() const { return arguments_; }
    // The type of the last argument: draw, indexed draw or dispatch.
    D3D12_INDIRECT_ARGUMENT_TYPE action() const { return arguments_.back().Type; }
    // Bytes one command's arguments take up in the argument buffer.
    UINT argument_bytes() const { return argument_bytes_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandSignature>(this, riid, out);
    }

private:
    explicit CommandSignature(Device *device) : ChildImpl(device) {}
    ~CommandSignature() override { safe_release(root_signature_); }

    UINT stride_ = 0;
    UINT argument_bytes_ = 0;
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments_;
    RootSignature *root_signature_ = nullptr;  // owned reference
};

// Bytes an argument takes in the argument buffer (zero for arguments that read nothing from it).
inline UINT indirect_argument_size(const D3D12_INDIRECT_ARGUMENT_DESC &argument)
{
    switch (argument.Type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: return 16;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: return 20;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: return 12;
    case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
    case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: return 16;
    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: return argument.Constant.Num32BitValuesToSet * 4;
    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
    case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
    case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: return 8;
    default: return 0;
    }
}

} // namespace d3d12m
