#include "d3d12/root_signature.h"

#include <cstring>

#include "d3d12/device.h"
#include "d3d12/root_signature_blob.h"
#include "d3d12/sampler.h"

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
    const UINT num_samplers = parsed.desc11.NumStaticSamplers;

    // The shader converter has no static samplers. Turn them into one more descriptor table, a
    // range per sampler at the sampler's own register and space, over a sampler heap this root
    // signature fills at creation and points the table at (see init_arguments).
    std::vector<uint8_t> rewritten;
    std::vector<D3D12_ROOT_PARAMETER1> params(parsed.desc11.pParameters,
                                              parsed.desc11.pParameters + parsed.desc11.NumParameters);
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;
    if (num_samplers) {
        for (UINT i = 0; i < num_samplers; ++i) {
            const D3D12_STATIC_SAMPLER_DESC &s = parsed.desc11.pStaticSamplers[i];
            D3D12_DESCRIPTOR_RANGE1 range = {};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = s.ShaderRegister;
            range.RegisterSpace = s.RegisterSpace;
            range.OffsetInDescriptorsFromTableStart = i;
            ranges.push_back(range);
        }
        D3D12_ROOT_PARAMETER1 table = {};
        table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        table.DescriptorTable = {num_samplers, ranges.data()};
        table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(table);

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1 = {static_cast<UINT>(params.size()), params.data(), 0, nullptr, parsed.desc11.Flags};
        std::string error;
        hr = serialize_root_signature(desc, D3D_ROOT_SIGNATURE_VERSION_1_1, rewritten, &error);
        if (FAILED(hr)) {
            D3D12M_LOG("root signature with static samplers: %s", error.c_str());
            rs->Release();
            return hr;
        }

        mtlb_buffer_info info;
        mtlb_result result = mtlb_descriptor_heap_create(device->handle(), num_samplers, &rs->static_samplers_, &info);
        if (result != MTLB_OK) {
            rs->Release();
            return to_hresult(result);
        }
        auto *entries = static_cast<mtlb_descriptor *>(info.cpu_ptr);
        for (UINT i = 0; i < num_samplers && result == MTLB_OK; ++i) {
            const mtlb_sampler_desc sampler = to_sampler_desc(parsed.desc11.pStaticSamplers[i]);
            result = mtlb_sampler_create(device->handle(), &sampler, &entries[i]);
        }
        if (result != MTLB_OK) {
            D3D12M_LOG("static sampler creation failed: %s", mtlb_last_error());
            rs->Release();
            return to_hresult(result);
        }
        rs->static_samplers_address_ = info.gpu_address;
    }

    const void *converter_blob = num_samplers ? rewritten.data() : blob;
    const size_t converter_size = num_samplers ? rewritten.size() : size;
    mtlb_root_signature_layout layout;
    mtlb_result result = mtlb_root_signature_create(device->handle(), converter_blob, converter_size, &rs->handle_, &layout);
    if (result == MTLB_OK && layout.num_parameters != params.size())
        result = MTLB_ERROR_COMPILE_FAILED;
    if (result != MTLB_OK) {
        D3D12M_LOG("root signature creation failed: %s", mtlb_last_error());
        rs->Release();
        return to_hresult(result);
    }
    for (UINT i = 0; i < parsed.desc11.NumParameters; ++i)
        rs->slots_.push_back({parsed.desc11.pParameters[i].ParameterType, layout.parameters[i].offset,
                              layout.parameters[i].size});
    if (num_samplers)
        rs->static_samplers_offset_ = layout.parameters[parsed.desc11.NumParameters].offset;
    rs->argument_buffer_size_ = layout.argument_buffer_size;

    return hand_out(rs, riid, out);
}

void RootSignature::init_arguments(uint8_t *arguments) const
{
    if (static_samplers_)
        std::memcpy(arguments + static_samplers_offset_, &static_samplers_address_, sizeof(static_samplers_address_));
}

RootSignature::~RootSignature()
{
    if (handle_)
        mtlb_root_signature_destroy(handle_);
    if (static_samplers_)
        mtlb_buffer_destroy(static_samplers_);
}

} // namespace d3d12m
