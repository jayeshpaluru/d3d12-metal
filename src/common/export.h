// Marks the C entry points the layer exports (the library is built with
// -fvisibility=hidden).
#pragma once

#define D3D12M_EXPORT extern "C" __attribute__((visibility("default")))
