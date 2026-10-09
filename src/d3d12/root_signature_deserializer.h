// SPDX-License-Identifier: LGPL-2.1-or-later
// ID3D12RootSignatureDeserializer / ID3D12VersionedRootSignatureDeserializer.
#pragma once

#include <cstddef>

#include "common/d3d12_uuids.h"

namespace d3d12m {

// Parses `data` (a DXBC container with an RTS0 part, or a bare RTS0 payload)
// and returns the deserializer interface `riid` (either of the two interfaces
// above).
HRESULT create_root_signature_deserializer(const void *data, size_t size, REFIID riid, void **out);

} // namespace d3d12m
