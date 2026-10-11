// SPDX-License-Identifier: LGPL-2.1-or-later
// D3D12 sampler descriptions in the bridge's form.
#pragma once

#include <algorithm>
#include <cmath>

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

// Out-of-range values are what the debug layer rejects and a driver tolerates: clamp them to the nearest legal
// value so Metal only ever sees a valid sampler (and a hostile description cannot be turned into a bad Metal call).
inline void sanitize(mtlb_sampler_desc &s)
{
    auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    for (uint32_t *mode : {&s.address_u, &s.address_v, &s.address_w})
        if (*mode < 1 || *mode > 5)
            *mode = 1;
    s.max_anisotropy = std::clamp<uint32_t>(s.max_anisotropy, 1, 16);
    if (s.compare_func > 8)
        s.compare_func = 8;
    s.mip_lod_bias = std::clamp(finite(s.mip_lod_bias, 0.0f), -16.0f, 15.99f);
    s.min_lod = std::clamp(finite(s.min_lod, 0.0f), 0.0f, 1000.0f);
    s.max_lod = std::clamp(finite(s.max_lod, 1000.0f), s.min_lod, 1000.0f);
    for (float &c : s.border_color)
        c = finite(c, 0.0f);
}

// D3D12_FILTER: bit 0 mip, bits 2-3 mag, bits 4-5 min (0 point, 1 linear), bit 6 anisotropic,
// bits 7-8 the reduction (0 average, 1 comparison, 2 minimum, 3 maximum).
inline mtlb_sampler_desc to_sampler_desc(const D3D12_SAMPLER_DESC &d)
{
    const UINT filter = d.Filter;
    mtlb_sampler_desc s{};
    s.min_filter = (filter >> 4) & 1;
    s.mag_filter = (filter >> 2) & 1;
    s.mip_filter = filter & 1;
    s.address_u = d.AddressU;
    s.address_v = d.AddressV;
    s.address_w = d.AddressW;
    s.mip_lod_bias = d.MipLODBias;
    s.max_anisotropy = (filter & 0x40) ? d.MaxAnisotropy : 1;
    const UINT reduction = (filter >> 7) & 3;
    s.compare_func = reduction == 1 ? d.ComparisonFunc : 0;
    s.reduction = reduction >= 2 ? reduction - 1 : 0;
    std::copy_n(d.BorderColor, 4, s.border_color);
    s.min_lod = d.MinLOD;
    s.max_lod = d.MaxLOD;
    sanitize(s);
    return s;
}

inline mtlb_sampler_desc to_sampler_desc(const D3D12_STATIC_SAMPLER_DESC &d)
{
    D3D12_SAMPLER_DESC plain{};
    plain.Filter = d.Filter;
    plain.AddressU = d.AddressU;
    plain.AddressV = d.AddressV;
    plain.AddressW = d.AddressW;
    plain.MipLODBias = d.MipLODBias;
    plain.MaxAnisotropy = d.MaxAnisotropy;
    plain.ComparisonFunc = d.ComparisonFunc;
    switch (d.BorderColor) {
    case D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK: plain.BorderColor[3] = 1; break;
    case D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE: std::fill_n(plain.BorderColor, 4, 1.0f); break;
    default: break;
    }
    plain.MinLOD = d.MinLOD;
    plain.MaxLOD = d.MaxLOD;
    return to_sampler_desc(plain);
}

} // namespace d3d12m
