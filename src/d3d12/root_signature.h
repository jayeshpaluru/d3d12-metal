// ID3D12RootSignature and its top-level argument buffer layout.
#pragma once

#include <vector>

#include "d3d12/object.h"

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

    const std::vector<Slot> &slots() const { return slots_; }
    uint32_t argument_buffer_size() const { return argument_buffer_size_; }
    // The serialized root signature as given to CreateRootSignature.
    const std::vector<uint8_t> &blob() const { return blob_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12RootSignature>(this, riid, out);
    }

private:
    explicit RootSignature(Device *device) : ChildImpl(device) {}

    std::vector<uint8_t> blob_;
    std::vector<Slot> slots_;
    uint32_t argument_buffer_size_ = 0;
};

} // namespace d3d12m
