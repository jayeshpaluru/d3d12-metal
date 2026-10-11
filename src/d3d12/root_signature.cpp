// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12/root_signature.h"

#include <climits>
#include <cstring>

#include "common/config.h"

#include "d3d12/descriptor_heap.h"
#include "d3d12/device.h"
#include "d3d12/root_signature_blob.h"
#include "d3d12/sampler.h"

namespace d3d12m {

namespace {

// The window table of each descriptor table with SRV or UAV ranges, appended after `params` (see MTLB_TYPED_WINDOWS
// in mtlb.h): range [base, base + n) of space s at table offset o becomes [3 * base, 3 * (base + n)) of space
// s + MTLB_WINDOW_SPACE_OFFSET at offset 3 * o. `window_param[i]` is the index of table i's window table, or -1.
// Returns false when no table needs one.
bool append_window_tables(std::vector<D3D12_ROOT_PARAMETER1> &params, UINT app_params,
                          std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> &storage, std::vector<int> &window_param)
{
    constexpr UINT kScale = MTLB_TYPED_WINDOWS;
    window_param.assign(app_params, -1);
    storage.clear();
    storage.reserve(app_params);
    for (UINT i = 0; i < app_params; ++i) {
        const D3D12_ROOT_PARAMETER1 &p = params[i];
        if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            continue;
        std::vector<D3D12_DESCRIPTOR_RANGE1> windows;
        uint64_t next = 0;  // where an APPEND range starts
        for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
            const D3D12_DESCRIPTOR_RANGE1 &range = p.DescriptorTable.pDescriptorRanges[r];
            const uint64_t offset = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                        ? next : range.OffsetInDescriptorsFromTableStart;
            const bool unbounded = range.NumDescriptors == UINT_MAX;
            next = unbounded ? UINT32_MAX : offset + range.NumDescriptors;
            if (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SRV && range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_UAV)
                continue;
            const uint64_t base = uint64_t(range.BaseShaderRegister) * kScale;
            const uint64_t count = unbounded ? UINT_MAX : uint64_t(range.NumDescriptors) * kScale;
            const uint64_t space = uint64_t(range.RegisterSpace) + MTLB_WINDOW_SPACE_OFFSET;
            if (base + (unbounded ? 0 : count) > UINT32_MAX || offset * kScale >= UINT32_MAX || space >= 0xfffffff0u)
                continue;  // (registers or offsets too high to scale: those views keep their first window only)
            D3D12_DESCRIPTOR_RANGE1 w = range;
            w.BaseShaderRegister = static_cast<UINT>(base);
            w.NumDescriptors = static_cast<UINT>(count);
            w.RegisterSpace = static_cast<UINT>(space);
            w.OffsetInDescriptorsFromTableStart = static_cast<UINT>(offset * kScale);
            windows.push_back(w);
        }
        if (windows.empty())
            continue;
        storage.push_back(std::move(windows));
        D3D12_ROOT_PARAMETER1 table = {};
        table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        table.DescriptorTable = {static_cast<UINT>(storage.back().size()), storage.back().data()};
        table.ShaderVisibility = p.ShaderVisibility;
        window_param[i] = static_cast<int>(params.size());
        params.push_back(table);
    }
    return !storage.empty();
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
    rs->content_key_ = root_signature_key(blob, size);
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

    // The windowed variant: the same parameters (at the same places in the argument buffer) and the window tables.
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> window_ranges;
    std::vector<int> window_param;
    const UINT base_params = static_cast<UINT>(params.size());
    if (typed_windows_enabled() && append_window_tables(params, parsed.desc11.NumParameters, window_ranges, window_param)) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1 = {static_cast<UINT>(params.size()), params.data(), 0, nullptr, parsed.desc11.Flags};
        std::vector<uint8_t> windowed_blob;
        std::string error;
        mtlb_root_signature_layout windowed_layout;
        bool ok = SUCCEEDED(serialize_root_signature(desc, D3D_ROOT_SIGNATURE_VERSION_1_1, windowed_blob, &error))
                  && mtlb_root_signature_create(device->handle(), windowed_blob.data(), windowed_blob.size(), &rs->windowed_,
                                                &windowed_layout) == MTLB_OK
                  && windowed_layout.num_parameters == params.size();
        for (UINT i = 0; ok && i < base_params; ++i)
            ok = windowed_layout.parameters[i].offset == layout.parameters[i].offset;
        if (ok)
            ok = mtlb_root_signature_set_windowed(rs->handle_, rs->windowed_) == MTLB_OK;
        if (ok) {
            for (UINT i = 0; i < parsed.desc11.NumParameters; ++i)
                if (window_param[i] >= 0)
                    rs->slots_[i].window_offset = windowed_layout.parameters[window_param[i]].offset;
            rs->argument_buffer_size_ = windowed_layout.argument_buffer_size;
        } else {
            D3D12M_LOG("root signature: no window tables (%s): typed buffer views keep their first 2^%d elements",
                       error.empty() ? mtlb_last_error() : error.c_str(), MTLB_TYPED_WINDOW_BITS);
        }
    }

    return hand_out(rs, riid, out);
}

void RootSignature::init_arguments(uint8_t *arguments) const
{
    if (static_samplers_)
        std::memcpy(arguments + static_samplers_offset_, &static_samplers_address_, sizeof(static_samplers_address_));
}

HRESULT RootSignature::acquire_embedded(Device *device, const void *shader, size_t size, RootSignature **out)
{
    *out = nullptr;
    if (!shader || !size)
        return E_INVALIDARG;
    const uint8_t *payload;
    size_t payload_size;
    if (FAILED(root_signature_payload(shader, size, &payload, &payload_size)))
        return E_INVALIDARG;
    const RootSignatureKey key = root_signature_key(shader, size);
    if ((*out = device->find_embedded_root_signature(key)))
        return S_OK;
    ID3D12RootSignature *created = nullptr;
    HRESULT hr = create(device, shader, size, __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&created));
    if (FAILED(hr))
        return hr;
    auto *rs = ours<RootSignature>(created);
    rs->shared_ = true;
    device->add_embedded_root_signature(rs);
    rs->add_internal_ref();
    created->Release();
    *out = rs;
    return S_OK;
}

RootSignature::~RootSignature()
{
    if (shared_)
        device()->remove_embedded_root_signature(this);
    if (handle_)
        mtlb_root_signature_destroy(handle_);
    if (windowed_)  // (after the root signature that uses it)
        mtlb_root_signature_destroy(windowed_);
    if (static_samplers_)
        mtlb_buffer_destroy(static_samplers_);
}

} // namespace d3d12m
