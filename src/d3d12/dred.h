// Device Removed Extended Data: accepted and empty. The layer never reports a
// removed device, so there is nothing to record; applications that opt in
// (D3D12GetDebugInterface) or query the device get objects that say so.
#pragma once

#include "d3d12/object.h"

namespace d3d12m {

// Static objects with no-op reference counting; they live as long as the module.
ID3D12DeviceRemovedExtendedDataSettings1 *dred_settings();
ID3D12DeviceRemovedExtendedData2 *dred_data();

} // namespace d3d12m
