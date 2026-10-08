#include "d3d12/root_signature.h"

#include "d3d12/root_signature_blob.h"

namespace d3d12m {

namespace {

uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

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
    rs->blob_.assign(static_cast<const uint8_t *>(blob), static_cast<const uint8_t *>(blob) + size);

    // Metal shader converter layout: parameters in order, root constants inline
    // (4-byte aligned), root descriptors and descriptor tables as 64-bit
    // addresses (8-byte aligned).
    uint32_t offset = 0;
    for (UINT i = 0; i < parsed.desc11.NumParameters; ++i) {
        const D3D12_ROOT_PARAMETER1 &p = parsed.desc11.pParameters[i];
        const bool constants = p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        const uint32_t slot_size = constants ? p.Constants.Num32BitValues * 4 : 8;
        offset = align_up(offset, constants ? 4 : 8);
        rs->slots_.push_back({p.ParameterType, offset, slot_size});
        offset += slot_size;
    }
    rs->argument_buffer_size_ = align_up(offset, 8);

    hr = rs->QueryInterface(riid, out);
    rs->Release();
    return hr;
}

} // namespace d3d12m
