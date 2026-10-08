#include "d3d12/root_signature.h"

#include "d3d12/device.h"
#include "d3d12/root_signature_blob.h"

namespace d3d12m {

HRESULT RootSignature::create(Device *device, const void *blob, size_t size, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!blob)
        return E_INVALIDARG;
    ParsedRootSignature parsed;
    HRESULT hr = parse_root_signature(blob, size, parsed);
    if (FAILED(hr))
        return hr;

    auto *rs = new RootSignature(device);
    mtlb_root_signature_layout layout;
    if (mtlb_root_signature_create(device->handle(), blob, size, &rs->handle_, &layout) != MTLB_OK
        || layout.num_parameters != parsed.desc11.NumParameters) {
        D3D12M_LOG("root signature creation failed: %s", mtlb_last_error());
        rs->Release();
        return E_FAIL;
    }
    for (UINT i = 0; i < layout.num_parameters; ++i)
        rs->slots_.push_back({parsed.desc11.pParameters[i].ParameterType, layout.parameters[i].offset,
                              layout.parameters[i].size});
    rs->argument_buffer_size_ = layout.argument_buffer_size;

    hr = rs->QueryInterface(riid, out);
    rs->Release();
    return hr;
}

RootSignature::~RootSignature()
{
    if (handle_)
        mtlb_root_signature_destroy(handle_);
}

} // namespace d3d12m
