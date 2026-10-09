// SPDX-License-Identifier: LGPL-2.1-or-later
// Exported root signature entry points of d3d12.dll.
#include <cstring>
#include <string>
#include <vector>

#include "common/blob.h"
#include "common/trace.h"
#include "common/export.h"
#include "d3d12/root_signature_blob.h"
#include "d3d12/root_signature_deserializer.h"

namespace {

// Serializes `desc` into *blob; on failure stores the message in *error_blob
// (when requested) and leaves *blob null.
HRESULT serialize_to_blobs(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &desc,
                           uint32_t target_version, ID3DBlob **blob,
                           ID3DBlob **error_blob)
{
    std::vector<uint8_t> bytes;
    std::string error;
    HRESULT hr = d3d12m::serialize_root_signature(desc, target_version, bytes, &error);
    if (SUCCEEDED(hr))
        return d3d12m::create_blob(bytes.data(), bytes.size(), blob);
    if (error_blob)
        d3d12m::create_blob_from_string(error, error_blob);
    return hr;
}

// Clears the output pointers before any validation so failures leave them null.
void clear_outputs(ID3DBlob **blob, ID3DBlob **error_blob)
{
    if (blob)
        *blob = nullptr;
    if (error_blob)
        *error_blob = nullptr;
}

} // namespace

D3D12M_EXPORT HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *root_signature,
                                                         D3D_ROOT_SIGNATURE_VERSION version,
                                                         ID3DBlob **blob, ID3DBlob **error_blob)
{
    D3D12M_TRACED_BEGIN
    clear_outputs(blob, error_blob);
    if (!root_signature || !blob)
        return E_INVALIDARG;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0 = *root_signature;
    return serialize_to_blobs(desc, static_cast<uint32_t>(version), blob, error_blob);
    D3D12M_TRACED_END(root_signature, version, blob, error_blob)
}

D3D12M_EXPORT HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature, ID3DBlob **blob, ID3DBlob **error_blob)
{
    D3D12M_TRACED_BEGIN
    clear_outputs(blob, error_blob);
    if (!root_signature || !blob)
        return E_INVALIDARG;
    uint32_t version;
    std::memcpy(&version, &root_signature->Version, sizeof(version));  // may hold an invalid enum value
    return serialize_to_blobs(*root_signature, version, blob, error_blob);
    D3D12M_TRACED_END(root_signature, blob, error_blob)
}

D3D12M_EXPORT HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T size,
                                                                  REFIID riid, void **deserializer)
{
    D3D12M_TRACED_BEGIN
    return d3d12m::create_root_signature_deserializer(data, size, riid, deserializer);
    D3D12M_TRACED_END(data, size, riid, deserializer)
}

D3D12M_EXPORT HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T size,
                                                                           REFIID riid, void **deserializer)
{
    D3D12M_TRACED_BEGIN
    return d3d12m::create_root_signature_deserializer(data, size, riid, deserializer);
    D3D12M_TRACED_END(data, size, riid, deserializer)
}
