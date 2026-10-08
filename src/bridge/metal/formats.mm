// The format table: for each mtlb_format (DXGI_FORMAT numbering) its block
// geometry, capability flags and Metal pixel and vertex formats. Everything
// else (mtlb_format_get_info, to_pixel_format, to_vertex_format) reads this one
// table, so the vertex capability flag cannot disagree with the vertex mapping.
#include "internal.h"

#include <iterator>

namespace {

constexpr uint32_t C = MTLB_FORMAT_FLAG_COLOR;
constexpr uint32_t T = MTLB_FORMAT_FLAG_TEXTURE;
constexpr uint32_t R = MTLB_FORMAT_FLAG_RENDER_TARGET;
constexpr uint32_t B = MTLB_FORMAT_FLAG_BLENDABLE;
constexpr uint32_t S = MTLB_FORMAT_FLAG_SRGB;
constexpr uint32_t W = MTLB_FORMAT_FLAG_SHADER_WRITE;
constexpr uint32_t BC = MTLB_FORMAT_FLAG_COMPRESSED | MTLB_FORMAT_FLAG_COLOR | MTLB_FORMAT_FLAG_TEXTURE;
constexpr uint32_t Y = MTLB_FORMAT_FLAG_TYPELESS;
constexpr uint32_t D = MTLB_FORMAT_FLAG_DEPTH;
constexpr uint32_t DS = MTLB_FORMAT_FLAG_DEPTH | MTLB_FORMAT_FLAG_STENCIL;

struct Entry {
    mtlb_format format;
    uint32_t block_width, block_height, bytes_per_block;
    uint32_t flags;  // MTLB_FORMAT_FLAG_*, except VERTEX, which follows from `vertex`
    MTLPixelFormat pixel;
    MTLVertexFormat vertex;
};

// Typeless formats are resources only: the pixel format is the one a texture is created with, views
// reinterpret it. Formats that exist only to view one plane of a depth-stencil texture
// (R32_FLOAT_X8X24_TYPELESS and friends) map to the depth-stencil texture's own format.
constexpr Entry kFormats[] = {
    {MTLB_FORMAT_R32G32B32A32_TYPELESS, 1, 1, 16, C | T | R | W | Y,
     MTLPixelFormatRGBA32Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R32G32B32_TYPELESS, 1, 1, 12, C | Y,
     MTLPixelFormatInvalid, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R16G16B16A16_TYPELESS, 1, 1, 8, C | T | R | B | W | Y,
     MTLPixelFormatRGBA16Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R32G32_TYPELESS, 1, 1, 8, C | T | R | W | Y,
     MTLPixelFormatRG32Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R32G8X24_TYPELESS, 1, 1, 8, DS | T | R | Y,
     MTLPixelFormatDepth32Float_Stencil8, MTLVertexFormatInvalid},
    {MTLB_FORMAT_D32_FLOAT_S8X24_UINT, 1, 1, 8, DS | T | R,
     MTLPixelFormatDepth32Float_Stencil8, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R32_FLOAT_X8X24_TYPELESS, 1, 1, 8, D | T | Y,
     MTLPixelFormatDepth32Float_Stencil8, MTLVertexFormatInvalid},
    {MTLB_FORMAT_X32_TYPELESS_G8X24_UINT, 1, 1, 8, MTLB_FORMAT_FLAG_STENCIL | T | Y,
     MTLPixelFormatDepth32Float_Stencil8, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R10G10B10A2_TYPELESS, 1, 1, 4, C | T | R | B | W | Y,
     MTLPixelFormatRGB10A2Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R16G16_TYPELESS, 1, 1, 4, C | T | R | B | W | Y,
     MTLPixelFormatRG16Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R32_TYPELESS, 1, 1, 4, C | T | R | W | Y,
     MTLPixelFormatR32Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R24G8_TYPELESS, 1, 1, 4, DS | T | R | Y,
     MTLPixelFormatDepth32Float_Stencil8, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R24_UNORM_X8_TYPELESS, 1, 1, 4, D | T | Y,
     MTLPixelFormatDepth32Float_Stencil8, MTLVertexFormatInvalid},
    {MTLB_FORMAT_X24_TYPELESS_G8_UINT, 1, 1, 4, MTLB_FORMAT_FLAG_STENCIL | T | Y,
     MTLPixelFormatDepth32Float_Stencil8, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R8G8_TYPELESS, 1, 1, 2, C | T | R | B | W | Y,
     MTLPixelFormatRG8Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R16_TYPELESS, 1, 1, 2, C | T | R | B | W | Y,
     MTLPixelFormatR16Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R8_TYPELESS, 1, 1, 1, C | T | R | B | W | Y,
     MTLPixelFormatR8Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_A8_UNORM, 1, 1, 1, C | T | R | B,
     MTLPixelFormatA8Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R9G9B9E5_SHAREDEXP, 1, 1, 4, C | T,
     MTLPixelFormatRGB9E5Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC1_TYPELESS, 4, 4, 8, BC | Y,
     MTLPixelFormatBC1_RGBA, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC2_TYPELESS, 4, 4, 16, BC | Y,
     MTLPixelFormatBC2_RGBA, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC3_TYPELESS, 4, 4, 16, BC | Y,
     MTLPixelFormatBC3_RGBA, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC4_TYPELESS, 4, 4, 8, BC | Y,
     MTLPixelFormatBC4_RUnorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC5_TYPELESS, 4, 4, 16, BC | Y,
     MTLPixelFormatBC5_RGUnorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_B5G6R5_UNORM, 1, 1, 2, C | T | R | B,
     MTLPixelFormatB5G6R5Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_B5G5R5A1_UNORM, 1, 1, 2, C | T | R | B,
     MTLPixelFormatBGR5A1Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC6H_TYPELESS, 4, 4, 16, BC | Y,
     MTLPixelFormatBC6H_RGBUfloat, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC7_TYPELESS, 4, 4, 16, BC | Y,
     MTLPixelFormatBC7_RGBAUnorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R32G32B32A32_FLOAT, 1, 1, 16, C | T | R | W,
     MTLPixelFormatRGBA32Float, MTLVertexFormatFloat4},
    {MTLB_FORMAT_R32G32B32A32_UINT, 1, 1, 16, C | T | R | W,
     MTLPixelFormatRGBA32Uint, MTLVertexFormatUInt4},
    {MTLB_FORMAT_R32G32B32A32_SINT, 1, 1, 16, C | T | R | W,
     MTLPixelFormatRGBA32Sint, MTLVertexFormatInt4},
    {MTLB_FORMAT_R32G32B32_FLOAT, 1, 1, 12, C,
     MTLPixelFormatInvalid, MTLVertexFormatFloat3},
    {MTLB_FORMAT_R32G32B32_UINT, 1, 1, 12, C,
     MTLPixelFormatInvalid, MTLVertexFormatUInt3},
    {MTLB_FORMAT_R32G32B32_SINT, 1, 1, 12, C,
     MTLPixelFormatInvalid, MTLVertexFormatInt3},
    {MTLB_FORMAT_R16G16B16A16_FLOAT, 1, 1, 8, C | T | R | B | W,
     MTLPixelFormatRGBA16Float, MTLVertexFormatHalf4},
    {MTLB_FORMAT_R16G16B16A16_UNORM, 1, 1, 8, C | T | R | B | W,
     MTLPixelFormatRGBA16Unorm, MTLVertexFormatUShort4Normalized},
    {MTLB_FORMAT_R16G16B16A16_UINT, 1, 1, 8, C | T | R | W,
     MTLPixelFormatRGBA16Uint, MTLVertexFormatUShort4},
    {MTLB_FORMAT_R16G16B16A16_SNORM, 1, 1, 8, C | T | R | B | W,
     MTLPixelFormatRGBA16Snorm, MTLVertexFormatShort4Normalized},
    {MTLB_FORMAT_R16G16B16A16_SINT, 1, 1, 8, C | T | R | W,
     MTLPixelFormatRGBA16Sint, MTLVertexFormatShort4},
    {MTLB_FORMAT_R32G32_FLOAT, 1, 1, 8, C | T | R | W,
     MTLPixelFormatRG32Float, MTLVertexFormatFloat2},
    {MTLB_FORMAT_R32G32_UINT, 1, 1, 8, C | T | R | W,
     MTLPixelFormatRG32Uint, MTLVertexFormatUInt2},
    {MTLB_FORMAT_R32G32_SINT, 1, 1, 8, C | T | R | W,
     MTLPixelFormatRG32Sint, MTLVertexFormatInt2},
    {MTLB_FORMAT_R10G10B10A2_UNORM, 1, 1, 4, C | T | R | B | W,
     MTLPixelFormatRGB10A2Unorm, MTLVertexFormatUInt1010102Normalized},
    {MTLB_FORMAT_R10G10B10A2_UINT, 1, 1, 4, C | T | R | W,
     MTLPixelFormatRGB10A2Uint, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R11G11B10_FLOAT, 1, 1, 4, C | T | R | B | W,
     MTLPixelFormatRG11B10Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R8G8B8A8_TYPELESS, 1, 1, 4, C | T | R | B | W | Y,
     MTLPixelFormatRGBA8Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, C | T | R | B | W,
     MTLPixelFormatRGBA8Unorm, MTLVertexFormatUChar4Normalized},
    {MTLB_FORMAT_R8G8B8A8_UNORM_SRGB, 1, 1, 4, C | T | R | B | S,
     MTLPixelFormatRGBA8Unorm_sRGB, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R8G8B8A8_UINT, 1, 1, 4, C | T | R | W,
     MTLPixelFormatRGBA8Uint, MTLVertexFormatUChar4},
    {MTLB_FORMAT_R8G8B8A8_SNORM, 1, 1, 4, C | T | R | B | W,
     MTLPixelFormatRGBA8Snorm, MTLVertexFormatChar4Normalized},
    {MTLB_FORMAT_R8G8B8A8_SINT, 1, 1, 4, C | T | R | W,
     MTLPixelFormatRGBA8Sint, MTLVertexFormatChar4},
    {MTLB_FORMAT_R16G16_FLOAT, 1, 1, 4, C | T | R | B | W,
     MTLPixelFormatRG16Float, MTLVertexFormatHalf2},
    {MTLB_FORMAT_R16G16_UNORM, 1, 1, 4, C | T | R | B | W,
     MTLPixelFormatRG16Unorm, MTLVertexFormatUShort2Normalized},
    {MTLB_FORMAT_R16G16_UINT, 1, 1, 4, C | T | R | W,
     MTLPixelFormatRG16Uint, MTLVertexFormatUShort2},
    {MTLB_FORMAT_R16G16_SNORM, 1, 1, 4, C | T | R | B | W,
     MTLPixelFormatRG16Snorm, MTLVertexFormatShort2Normalized},
    {MTLB_FORMAT_R16G16_SINT, 1, 1, 4, C | T | R | W,
     MTLPixelFormatRG16Sint, MTLVertexFormatShort2},
    {MTLB_FORMAT_D32_FLOAT, 1, 1, 4, D | T | R,
     MTLPixelFormatDepth32Float, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R32_FLOAT, 1, 1, 4, C | T | R | W,
     MTLPixelFormatR32Float, MTLVertexFormatFloat},
    {MTLB_FORMAT_R32_UINT, 1, 1, 4, C | T | R | W,
     MTLPixelFormatR32Uint, MTLVertexFormatUInt},
    {MTLB_FORMAT_R32_SINT, 1, 1, 4, C | T | R | W,
     MTLPixelFormatR32Sint, MTLVertexFormatInt},
    {MTLB_FORMAT_D24_UNORM_S8_UINT, 1, 1, 4, DS | T | R,
     MTLPixelFormatDepth32Float_Stencil8, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R8G8_UNORM, 1, 1, 2, C | T | R | B | W,
     MTLPixelFormatRG8Unorm, MTLVertexFormatUChar2Normalized},
    {MTLB_FORMAT_R8G8_UINT, 1, 1, 2, C | T | R | W,
     MTLPixelFormatRG8Uint, MTLVertexFormatUChar2},
    {MTLB_FORMAT_R8G8_SNORM, 1, 1, 2, C | T | R | B | W,
     MTLPixelFormatRG8Snorm, MTLVertexFormatChar2Normalized},
    {MTLB_FORMAT_R8G8_SINT, 1, 1, 2, C | T | R | W,
     MTLPixelFormatRG8Sint, MTLVertexFormatChar2},
    {MTLB_FORMAT_R16_FLOAT, 1, 1, 2, C | T | R | B | W,
     MTLPixelFormatR16Float, MTLVertexFormatHalf},
    {MTLB_FORMAT_D16_UNORM, 1, 1, 2, D | T | R,
     MTLPixelFormatDepth16Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_R16_UNORM, 1, 1, 2, C | T | R | B | W,
     MTLPixelFormatR16Unorm, MTLVertexFormatUShortNormalized},
    {MTLB_FORMAT_R16_UINT, 1, 1, 2, C | T | R | W,
     MTLPixelFormatR16Uint, MTLVertexFormatUShort},
    {MTLB_FORMAT_R16_SNORM, 1, 1, 2, C | T | R | B | W,
     MTLPixelFormatR16Snorm, MTLVertexFormatShortNormalized},
    {MTLB_FORMAT_R16_SINT, 1, 1, 2, C | T | R | W,
     MTLPixelFormatR16Sint, MTLVertexFormatShort},
    {MTLB_FORMAT_R8_UNORM, 1, 1, 1, C | T | R | B | W,
     MTLPixelFormatR8Unorm, MTLVertexFormatUCharNormalized},
    {MTLB_FORMAT_R8_UINT, 1, 1, 1, C | T | R | W,
     MTLPixelFormatR8Uint, MTLVertexFormatUChar},
    {MTLB_FORMAT_R8_SNORM, 1, 1, 1, C | T | R | B | W,
     MTLPixelFormatR8Snorm, MTLVertexFormatCharNormalized},
    {MTLB_FORMAT_R8_SINT, 1, 1, 1, C | T | R | W,
     MTLPixelFormatR8Sint, MTLVertexFormatChar},
    {MTLB_FORMAT_BC1_UNORM, 4, 4, 8, BC,
     MTLPixelFormatBC1_RGBA, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC1_UNORM_SRGB, 4, 4, 8, BC | S,
     MTLPixelFormatBC1_RGBA_sRGB, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC2_UNORM, 4, 4, 16, BC,
     MTLPixelFormatBC2_RGBA, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC2_UNORM_SRGB, 4, 4, 16, BC | S,
     MTLPixelFormatBC2_RGBA_sRGB, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC3_UNORM, 4, 4, 16, BC,
     MTLPixelFormatBC3_RGBA, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC3_UNORM_SRGB, 4, 4, 16, BC | S,
     MTLPixelFormatBC3_RGBA_sRGB, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC4_UNORM, 4, 4, 8, BC,
     MTLPixelFormatBC4_RUnorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC4_SNORM, 4, 4, 8, BC,
     MTLPixelFormatBC4_RSnorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC5_UNORM, 4, 4, 16, BC,
     MTLPixelFormatBC5_RGUnorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC5_SNORM, 4, 4, 16, BC,
     MTLPixelFormatBC5_RGSnorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_B8G8R8A8_UNORM, 1, 1, 4, C | T | R | B | W,
     MTLPixelFormatBGRA8Unorm, MTLVertexFormatUChar4Normalized_BGRA},
    {MTLB_FORMAT_B8G8R8A8_TYPELESS, 1, 1, 4, C | T | R | B | W | Y,
     MTLPixelFormatBGRA8Unorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_B8G8R8A8_UNORM_SRGB, 1, 1, 4, C | T | R | B | S,
     MTLPixelFormatBGRA8Unorm_sRGB, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC6H_UF16, 4, 4, 16, BC,
     MTLPixelFormatBC6H_RGBUfloat, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC6H_SF16, 4, 4, 16, BC,
     MTLPixelFormatBC6H_RGBFloat, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC7_UNORM, 4, 4, 16, BC,
     MTLPixelFormatBC7_RGBAUnorm, MTLVertexFormatInvalid},
    {MTLB_FORMAT_BC7_UNORM_SRGB, 4, 4, 16, BC | S,
     MTLPixelFormatBC7_RGBAUnorm_sRGB, MTLVertexFormatInvalid},
};

constexpr size_t kMaxFormat = 128;

// DXGI value -> index into kFormats, or -1.
constexpr std::array<int8_t, kMaxFormat> make_index()
{
    std::array<int8_t, kMaxFormat> index{};
    for (auto &i : index)
        i = -1;
    for (size_t i = 0; i < std::size(kFormats); ++i)
        index[kFormats[i].format] = static_cast<int8_t>(i);
    return index;
}

constexpr std::array<int8_t, kMaxFormat> kIndex = make_index();

const Entry *find_format(uint32_t format)
{
    return format < kMaxFormat && kIndex[format] >= 0 ? &kFormats[kIndex[format]] : nullptr;
}

} // namespace

namespace mtlb {

MTLPixelFormat to_pixel_format(uint32_t format)
{
    const Entry *e = find_format(format);
    return e ? e->pixel : MTLPixelFormatInvalid;
}

MTLVertexFormat to_vertex_format(uint32_t format)
{
    const Entry *e = find_format(format);
    return e ? e->vertex : MTLVertexFormatInvalid;
}

MTLPixelFormat to_texture_pixel_format(uint32_t format, bool depth_stencil_usage)
{
    const Entry *e = find_format(format);
    if (!e)
        return MTLPixelFormatInvalid;
    if (depth_stencil_usage && (e->flags & MTLB_FORMAT_FLAG_TYPELESS)) {
        switch (format) {
        case MTLB_FORMAT_R32_TYPELESS: return MTLPixelFormatDepth32Float;
        case MTLB_FORMAT_R16_TYPELESS: return MTLPixelFormatDepth16Unorm;
        default: break;  // R32G8X24 and R24G8 already map to a depth-stencil format
        }
    }
    return e->pixel;
}

MTLPixelFormat to_view_pixel_format(MTLPixelFormat base, uint32_t view_format)
{
    const Entry *e = find_format(view_format);
    if (!e)
        return MTLPixelFormatInvalid;
    const bool base_depth = base == MTLPixelFormatDepth32Float || base == MTLPixelFormatDepth16Unorm
                            || base == MTLPixelFormatDepth32Float_Stencil8 || base == MTLPixelFormatDepth24Unorm_Stencil8;
    if (!base_depth)
        return e->pixel;
    // A view of a depth-stencil texture reads one plane. The depth plane is the texture's own
    // format (shaders read it as a float texture); the stencil plane has a format of its own.
    if (e->flags & MTLB_FORMAT_FLAG_STENCIL && !(e->flags & MTLB_FORMAT_FLAG_DEPTH))
        return MTLPixelFormatX32_Stencil8;
    return base;
}

} // namespace mtlb

extern "C" mtlb_result mtlb_format_get_info(mtlb_format format, mtlb_format_info *out)
{
    if (!out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    const Entry *e = find_format(format);
    if (!e)
        return MTLB_ERROR_UNSUPPORTED;
    uint32_t flags = e->flags;
    if (e->vertex != MTLVertexFormatInvalid)
        flags |= MTLB_FORMAT_FLAG_VERTEX;
    // Texture buffers take uncompressed colour formats that are not sRGB.
    if (e->pixel != MTLPixelFormatInvalid && !(flags & (MTLB_FORMAT_FLAG_COMPRESSED | MTLB_FORMAT_FLAG_DEPTH | MTLB_FORMAT_FLAG_SRGB)))
        flags |= MTLB_FORMAT_FLAG_BUFFER;
    *out = {e->block_width, e->block_height, e->bytes_per_block, flags};
    return MTLB_OK;
}
