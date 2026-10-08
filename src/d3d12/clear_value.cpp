#include "d3d12/clear_value.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace d3d12m {

namespace {

enum class Type { Float, Unorm, Snorm, Uint, Sint };

struct Layout {
    DXGI_FORMAT format;
    uint8_t components;
    uint8_t bits;   // per component: 8, 16 or 32
    Type type;
};

constexpr Layout kLayouts[] = {
    {DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 32, Type::Float}, {DXGI_FORMAT_R32G32B32A32_UINT, 4, 32, Type::Uint},
    {DXGI_FORMAT_R32G32B32A32_SINT, 4, 32, Type::Sint},   {DXGI_FORMAT_R32G32B32A32_TYPELESS, 4, 32, Type::Uint},
    {DXGI_FORMAT_R32G32B32_FLOAT, 3, 32, Type::Float},    {DXGI_FORMAT_R32G32B32_UINT, 3, 32, Type::Uint},
    {DXGI_FORMAT_R32G32B32_SINT, 3, 32, Type::Sint},      {DXGI_FORMAT_R32G32_FLOAT, 2, 32, Type::Float},
    {DXGI_FORMAT_R32G32_UINT, 2, 32, Type::Uint},         {DXGI_FORMAT_R32G32_SINT, 2, 32, Type::Sint},
    {DXGI_FORMAT_R32_FLOAT, 1, 32, Type::Float},          {DXGI_FORMAT_R32_UINT, 1, 32, Type::Uint},
    {DXGI_FORMAT_R32_SINT, 1, 32, Type::Sint},            {DXGI_FORMAT_R32_TYPELESS, 1, 32, Type::Uint},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 16, Type::Float}, {DXGI_FORMAT_R16G16B16A16_UNORM, 4, 16, Type::Unorm},
    {DXGI_FORMAT_R16G16B16A16_SNORM, 4, 16, Type::Snorm}, {DXGI_FORMAT_R16G16B16A16_UINT, 4, 16, Type::Uint},
    {DXGI_FORMAT_R16G16B16A16_SINT, 4, 16, Type::Sint},   {DXGI_FORMAT_R16G16_FLOAT, 2, 16, Type::Float},
    {DXGI_FORMAT_R16G16_UNORM, 2, 16, Type::Unorm},       {DXGI_FORMAT_R16G16_SNORM, 2, 16, Type::Snorm},
    {DXGI_FORMAT_R16G16_UINT, 2, 16, Type::Uint},         {DXGI_FORMAT_R16G16_SINT, 2, 16, Type::Sint},
    {DXGI_FORMAT_R16_FLOAT, 1, 16, Type::Float},          {DXGI_FORMAT_R16_UNORM, 1, 16, Type::Unorm},
    {DXGI_FORMAT_R16_SNORM, 1, 16, Type::Snorm},          {DXGI_FORMAT_R16_UINT, 1, 16, Type::Uint},
    {DXGI_FORMAT_R16_SINT, 1, 16, Type::Sint},            {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 8, Type::Unorm},
    {DXGI_FORMAT_R8G8B8A8_SNORM, 4, 8, Type::Snorm},      {DXGI_FORMAT_R8G8B8A8_UINT, 4, 8, Type::Uint},
    {DXGI_FORMAT_R8G8B8A8_SINT, 4, 8, Type::Sint},        {DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, 8, Type::Uint},
    {DXGI_FORMAT_R8G8_UNORM, 2, 8, Type::Unorm},          {DXGI_FORMAT_R8G8_SNORM, 2, 8, Type::Snorm},
    {DXGI_FORMAT_R8G8_UINT, 2, 8, Type::Uint},            {DXGI_FORMAT_R8G8_SINT, 2, 8, Type::Sint},
    {DXGI_FORMAT_R8_UNORM, 1, 8, Type::Unorm},            {DXGI_FORMAT_R8_SNORM, 1, 8, Type::Snorm},
    {DXGI_FORMAT_R8_UINT, 1, 8, Type::Uint},              {DXGI_FORMAT_R8_SINT, 1, 8, Type::Sint},
};

uint16_t to_half(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exponent = int32_t((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (((bits >> 23) & 0xff) == 0xff)
        return static_cast<uint16_t>(sign | 0x7c00 | (mantissa ? 0x200 : 0));  // infinity, NaN
    if (exponent >= 31)
        return static_cast<uint16_t>(sign | 0x7c00);
    if (exponent <= 0) {
        if (exponent < -10)
            return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000) >> (1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000) >> 13));
    }
    return static_cast<uint16_t>(sign | (exponent << 10) | ((mantissa + 0x1000) >> 13));
}

float as_float(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// One component as the integer bits stored in `bits` bits.
uint32_t convert(uint32_t value, Type type, unsigned bits, bool from_float)
{
    const uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    if (!from_float)
        return value & mask;  // integer values keep the bits the component has
    const float f = as_float(value);
    switch (type) {
    case Type::Float:
        return bits == 32 ? value : to_half(f);
    case Type::Unorm: {
        const float clamped = std::isnan(f) ? 0.0f : std::clamp(f, 0.0f, 1.0f);
        return static_cast<uint32_t>(std::lround(double(clamped) * mask)) & mask;
    }
    case Type::Snorm: {
        const float clamped = std::isnan(f) ? 0.0f : std::clamp(f, -1.0f, 1.0f);
        const int32_t max = static_cast<int32_t>(mask >> 1);
        return static_cast<uint32_t>(static_cast<int32_t>(std::lround(double(clamped) * max))) & mask;
    }
    default:  // a float clear of an integer format truncates
        return static_cast<uint32_t>(static_cast<int64_t>(f)) & mask;
    }
}

// A float as an unsigned float with a 5-bit exponent and `mantissa_bits` (R11G11B10_FLOAT components): negative
// values and NaN clamp to zero, overflow to the largest finite value, and infinity stays infinity.
uint32_t to_small_float(float f, unsigned mantissa_bits)
{
    const uint32_t mantissa_mask = (1u << mantissa_bits) - 1;
    if (std::isnan(f) || f <= 0.0f)
        return 0;
    if (std::isinf(f))
        return 0x1fu << mantissa_bits;
    const uint32_t half = to_half(f);  // sign is 0 here: 5-bit exponent and a 10-bit mantissa
    if ((half & 0x7c00) == 0x7c00)
        return (0x1eu << mantissa_bits) | mantissa_mask;  // too large for a half: the largest finite value
    return half >> (10 - mantissa_bits);
}

} // namespace

uint32_t pack_clear_element(DXGI_FORMAT format, const uint32_t values[4], bool from_float, uint8_t out[16])
{
    std::memset(out, 0, 16);
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_UINT) {
        const bool unorm = format == DXGI_FORMAT_R10G10B10A2_UNORM;
        uint32_t packed = 0;
        for (int i = 0; i < 3; ++i)
            packed |= convert(values[i], unorm ? Type::Unorm : Type::Uint, 10, from_float) << (10 * i);
        packed |= convert(values[3], unorm ? Type::Unorm : Type::Uint, 2, from_float) << 30;
        std::memcpy(out, &packed, 4);
        return 4;
    }
    if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
        // Integer values keep their low bits (11, 11 and 10); float values are converted.
        const unsigned bits[3] = {11, 11, 10};
        uint32_t packed = 0, shift = 0;
        for (int i = 0; i < 3; ++i) {
            const uint32_t mask = (1u << bits[i]) - 1;
            const uint32_t v = from_float ? to_small_float(as_float(values[i]), bits[i] - 5) : values[i] & mask;
            packed |= (v & mask) << shift;
            shift += bits[i];
        }
        std::memcpy(out, &packed, 4);
        return 4;
    }
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        const uint32_t order[4] = {2, 1, 0, 3};
        for (int i = 0; i < 4; ++i)
            out[i] = static_cast<uint8_t>(convert(values[order[i]], Type::Unorm, 8, from_float));
        return 4;
    }
    for (const Layout &l : kLayouts) {
        if (l.format != format)
            continue;
        const unsigned bytes = l.bits / 8;
        for (unsigned c = 0; c < l.components; ++c) {
            const uint32_t v = convert(values[c], l.type, l.bits, from_float);
            std::memcpy(out + c * bytes, &v, bytes);  // little endian
        }
        return l.components * bytes;
    }
    return 0;
}

} // namespace d3d12m
