// Format properties that do not depend on Metal: block geometry and capability
// flags per mtlb_format. The Metal pixel/vertex format mapping lives in
// metal/formats.mm.
#include "mtlb.h"

namespace {

constexpr uint32_t C = MTLB_FORMAT_FLAG_COLOR;
constexpr uint32_t T = MTLB_FORMAT_FLAG_TEXTURE;
constexpr uint32_t R = MTLB_FORMAT_FLAG_RENDER_TARGET;
constexpr uint32_t B = MTLB_FORMAT_FLAG_BLENDABLE;
constexpr uint32_t V = MTLB_FORMAT_FLAG_VERTEX;
constexpr uint32_t S = MTLB_FORMAT_FLAG_SRGB;
constexpr uint32_t W = MTLB_FORMAT_FLAG_SHADER_WRITE;
constexpr uint32_t BC = MTLB_FORMAT_FLAG_COMPRESSED | MTLB_FORMAT_FLAG_COLOR | MTLB_FORMAT_FLAG_TEXTURE;
constexpr uint32_t Y = MTLB_FORMAT_FLAG_TYPELESS;

struct Entry {
    mtlb_format format;
    mtlb_format_info info;
};

constexpr Entry kFormats[] = {
    {MTLB_FORMAT_R32G32B32A32_FLOAT, {1, 1, 16, C | T | R | W | V}},
    {MTLB_FORMAT_R32G32B32A32_UINT, {1, 1, 16, C | T | R | W | V}},
    {MTLB_FORMAT_R32G32B32A32_SINT, {1, 1, 16, C | T | R | W | V}},
    {MTLB_FORMAT_R32G32B32_FLOAT, {1, 1, 12, C | V}},
    {MTLB_FORMAT_R32G32B32_UINT, {1, 1, 12, C | V}},
    {MTLB_FORMAT_R32G32B32_SINT, {1, 1, 12, C | V}},
    {MTLB_FORMAT_R16G16B16A16_FLOAT, {1, 1, 8, C | T | R | B | W | V}},
    {MTLB_FORMAT_R16G16B16A16_UNORM, {1, 1, 8, C | T | R | B | W | V}},
    {MTLB_FORMAT_R16G16B16A16_UINT, {1, 1, 8, C | T | R | W | V}},
    {MTLB_FORMAT_R16G16B16A16_SNORM, {1, 1, 8, C | T | R | B | W | V}},
    {MTLB_FORMAT_R16G16B16A16_SINT, {1, 1, 8, C | T | R | W | V}},
    {MTLB_FORMAT_R32G32_FLOAT, {1, 1, 8, C | T | R | W | V}},
    {MTLB_FORMAT_R32G32_UINT, {1, 1, 8, C | T | R | W | V}},
    {MTLB_FORMAT_R32G32_SINT, {1, 1, 8, C | T | R | W | V}},
    {MTLB_FORMAT_R10G10B10A2_UNORM, {1, 1, 4, C | T | R | B | W | V}},
    {MTLB_FORMAT_R10G10B10A2_UINT, {1, 1, 4, C | T | R | W}},
    {MTLB_FORMAT_R11G11B10_FLOAT, {1, 1, 4, C | T | R | B | W}},
    {MTLB_FORMAT_R8G8B8A8_TYPELESS, {1, 1, 4, C | T | R | B | W | Y}},
    {MTLB_FORMAT_R8G8B8A8_UNORM, {1, 1, 4, C | T | R | B | W | V}},
    {MTLB_FORMAT_R8G8B8A8_UNORM_SRGB, {1, 1, 4, C | T | R | B | S}},
    {MTLB_FORMAT_R8G8B8A8_UINT, {1, 1, 4, C | T | R | W | V}},
    {MTLB_FORMAT_R8G8B8A8_SNORM, {1, 1, 4, C | T | R | B | W | V}},
    {MTLB_FORMAT_R8G8B8A8_SINT, {1, 1, 4, C | T | R | W | V}},
    {MTLB_FORMAT_R16G16_FLOAT, {1, 1, 4, C | T | R | B | W | V}},
    {MTLB_FORMAT_R16G16_UNORM, {1, 1, 4, C | T | R | B | W | V}},
    {MTLB_FORMAT_R16G16_UINT, {1, 1, 4, C | T | R | W | V}},
    {MTLB_FORMAT_R16G16_SNORM, {1, 1, 4, C | T | R | B | W | V}},
    {MTLB_FORMAT_R16G16_SINT, {1, 1, 4, C | T | R | W | V}},
    {MTLB_FORMAT_D32_FLOAT, {1, 1, 4, MTLB_FORMAT_FLAG_DEPTH | T | R}},
    {MTLB_FORMAT_R32_FLOAT, {1, 1, 4, C | T | R | W | V}},
    {MTLB_FORMAT_R32_UINT, {1, 1, 4, C | T | R | W | V}},
    {MTLB_FORMAT_R32_SINT, {1, 1, 4, C | T | R | W | V}},
    {MTLB_FORMAT_D24_UNORM_S8_UINT, {1, 1, 4, MTLB_FORMAT_FLAG_DEPTH | MTLB_FORMAT_FLAG_STENCIL | T | R}},
    {MTLB_FORMAT_R8G8_UNORM, {1, 1, 2, C | T | R | B | W | V}},
    {MTLB_FORMAT_R8G8_UINT, {1, 1, 2, C | T | R | W | V}},
    {MTLB_FORMAT_R8G8_SNORM, {1, 1, 2, C | T | R | B | W | V}},
    {MTLB_FORMAT_R8G8_SINT, {1, 1, 2, C | T | R | W | V}},
    {MTLB_FORMAT_R16_FLOAT, {1, 1, 2, C | T | R | B | W | V}},
    {MTLB_FORMAT_D16_UNORM, {1, 1, 2, MTLB_FORMAT_FLAG_DEPTH | T | R}},
    {MTLB_FORMAT_R16_UNORM, {1, 1, 2, C | T | R | B | W | V}},
    {MTLB_FORMAT_R16_UINT, {1, 1, 2, C | T | R | W | V}},
    {MTLB_FORMAT_R16_SNORM, {1, 1, 2, C | T | R | B | W | V}},
    {MTLB_FORMAT_R16_SINT, {1, 1, 2, C | T | R | W | V}},
    {MTLB_FORMAT_R8_UNORM, {1, 1, 1, C | T | R | B | W | V}},
    {MTLB_FORMAT_R8_UINT, {1, 1, 1, C | T | R | W | V}},
    {MTLB_FORMAT_R8_SNORM, {1, 1, 1, C | T | R | B | W | V}},
    {MTLB_FORMAT_R8_SINT, {1, 1, 1, C | T | R | W | V}},
    {MTLB_FORMAT_BC1_UNORM, {4, 4, 8, BC}},
    {MTLB_FORMAT_BC1_UNORM_SRGB, {4, 4, 8, BC | S}},
    {MTLB_FORMAT_BC2_UNORM, {4, 4, 16, BC}},
    {MTLB_FORMAT_BC2_UNORM_SRGB, {4, 4, 16, BC | S}},
    {MTLB_FORMAT_BC3_UNORM, {4, 4, 16, BC}},
    {MTLB_FORMAT_BC3_UNORM_SRGB, {4, 4, 16, BC | S}},
    {MTLB_FORMAT_BC4_UNORM, {4, 4, 8, BC}},
    {MTLB_FORMAT_BC4_SNORM, {4, 4, 8, BC}},
    {MTLB_FORMAT_BC5_UNORM, {4, 4, 16, BC}},
    {MTLB_FORMAT_BC5_SNORM, {4, 4, 16, BC}},
    {MTLB_FORMAT_B8G8R8A8_UNORM, {1, 1, 4, C | T | R | B | W | V}},
    {MTLB_FORMAT_B8G8R8A8_TYPELESS, {1, 1, 4, C | T | R | B | W | Y}},
    {MTLB_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 1, 4, C | T | R | B | S}},
    {MTLB_FORMAT_BC6H_UF16, {4, 4, 16, BC}},
    {MTLB_FORMAT_BC6H_SF16, {4, 4, 16, BC}},
    {MTLB_FORMAT_BC7_UNORM, {4, 4, 16, BC}},
    {MTLB_FORMAT_BC7_UNORM_SRGB, {4, 4, 16, BC | S}},
};

} // namespace

extern "C" mtlb_result mtlb_format_get_info(mtlb_format format, mtlb_format_info *out)
{
    if (!out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    for (const Entry &e : kFormats) {
        if (e.format == format) {
            *out = e.info;
            return MTLB_OK;
        }
    }
    return MTLB_ERROR_UNSUPPORTED;
}
