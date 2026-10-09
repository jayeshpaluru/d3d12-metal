// SPDX-License-Identifier: LGPL-2.1-or-later
// ID3D12RootSignature and its top-level argument buffer layout.
#pragma once

#include <vector>

#include "bridge/mtlb.h"
#include "d3d12/object.h"
#include "d3d12/root_signature_blob.h"

namespace d3d12m {

class RootSignature final : public ChildImpl<ID3D12RootSignature> {
public:
    // Where one root parameter lives in the top-level argument buffer.
    struct Slot {
        D3D12_ROOT_PARAMETER_TYPE type;
        uint32_t offset;  // bytes from the start of the argument buffer
        uint32_t size;    // bytes: 4 * Num32BitValues for constants, 8 otherwise
    };

    static HRESULT create(Device *device, const void *blob, size_t size, REFIID riid, void **out);
    // The root signature embedded in a shader (the RTS0 part of its container): one object per distinct payload, shared
    // by every pipeline that embeds it (so the converted shaders, which are keyed by root signature, are found again).
    // Returns it with an internal reference (release_internal_ref).
    static HRESULT acquire_embedded(Device *device, const void *shader, size_t size, RootSignature **out);

    const RootSignatureKey &content_key() const { return content_key_; }

    // The application's root parameters, by index.
    const std::vector<Slot> &slots() const { return slots_; }
    // Writes what the layer adds to a fresh argument buffer (the table that stands in for
    // static samplers, which the shader converter does not support).
    void init_arguments(uint8_t *arguments) const;
    uint32_t argument_buffer_size() const { return argument_buffer_size_; }
    mtlb_root_signature handle() const { return handle_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12RootSignature>(this, riid, out);
    }

private:
    explicit RootSignature(Device *device) : ChildImpl(device) {}
    ~RootSignature() override;

    mtlb_root_signature handle_ = 0;
    RootSignatureKey content_key_;
    bool shared_ = false;  // registered in the device's embedded root signatures
    // Static samplers become a descriptor table, appended after the application's parameters, over
    // a sampler heap of the root signature's own.
    mtlb_buffer static_samplers_ = 0;
    uint64_t static_samplers_address_ = 0;
    uint32_t static_samplers_offset_ = 0;  // of the table's slot in the argument buffer
    std::vector<Slot> slots_;
    uint32_t argument_buffer_size_ = 0;
};

} // namespace d3d12m
