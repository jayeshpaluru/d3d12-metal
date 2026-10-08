// Root signature serialization: the DXBC container with an "RTS0" part that
// D3D12SerializeRootSignature produces (versions 1.0 and 1.1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/d3d12_uuids.h"

namespace d3d12m {

// A parsed (or copied) root signature that owns all of its storage. It exposes
// the same signature as 1.0 and as 1.1 descriptions plus the description at
// the version it was stored in. Moving keeps every internal pointer valid
// because the pointers target heap buffers owned by the vectors.
struct ParsedRootSignature {
    ParsedRootSignature() = default;
    ParsedRootSignature(const ParsedRootSignature &) = delete;
    ParsedRootSignature &operator=(const ParsedRootSignature &) = delete;
    ParsedRootSignature(ParsedRootSignature &&) = default;
    ParsedRootSignature &operator=(ParsedRootSignature &&) = default;

    // Description at the version stored in the blob (Version is 1.0 or 1.1).
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC original{};
    // The signature in 1.0 form (descriptor flags dropped).
    D3D12_ROOT_SIGNATURE_DESC desc10{};
    // The signature in 1.1 form. A 1.0 source gets the 1.0 default flags:
    // volatile descriptors and data for CBV/SRV/UAV ranges (volatile descriptors
    // for sampler ranges) and DATA_VOLATILE for root descriptors.
    D3D12_ROOT_SIGNATURE_DESC1 desc11{};

    // Backing storage for the descriptions above.
    std::vector<D3D12_ROOT_PARAMETER> params10;
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges10;
    std::vector<D3D12_ROOT_PARAMETER1> params11;
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges11;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
};

// Serializes `desc` into a DXBC container (with a valid checksum) whose RTS0
// payload has version `target_version` (1.0 or 1.1). The description is
// converted if its version differs: 1.1 -> 1.0 drops descriptor flags, 1.0 ->
// 1.1 applies the 1.0 default flags. Returns E_INVALIDARG for a malformed
// description or unsupported version, with a short message in `error` when it
// is non-null.
HRESULT serialize_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &desc,
                                 uint32_t target_version,
                                 std::vector<uint8_t> &out, std::string *error);

// Parses a DXBC container holding an RTS0 part, or a bare RTS0 payload. A
// container whose checksum is non-zero must match the computed checksum (an
// all-zero checksum marks an unsigned container and is accepted). Every offset
// is bounds-checked; malformed input yields E_INVALIDARG.
HRESULT parse_root_signature(const void *blob, size_t size, ParsedRootSignature &out);

} // namespace d3d12m
