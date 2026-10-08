// Conversion between Metal registry ids and the LUIDs DXGI and D3D12 report.
#pragma once

#include <cstdint>

#include "common/com.h"

namespace d3d12m {

inline LUID luid_from_registry_id(uint64_t registry_id)
{
    return {static_cast<DWORD>(registry_id), static_cast<LONG>(registry_id >> 32)};
}

inline uint64_t registry_id_from_luid(LUID luid)
{
    return (uint64_t(static_cast<uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}

} // namespace d3d12m
