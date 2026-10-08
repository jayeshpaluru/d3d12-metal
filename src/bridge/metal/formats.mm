// mtlb_format (DXGI_FORMAT numbering) to Metal pixel and vertex formats.
#include "internal.h"

namespace mtlb {

MTLPixelFormat to_pixel_format(uint32_t format)
{
    switch (static_cast<mtlb_format>(format)) {
    case MTLB_FORMAT_R32G32B32A32_FLOAT: return MTLPixelFormatRGBA32Float;
    case MTLB_FORMAT_R32G32B32A32_UINT: return MTLPixelFormatRGBA32Uint;
    case MTLB_FORMAT_R32G32B32A32_SINT: return MTLPixelFormatRGBA32Sint;
    case MTLB_FORMAT_R16G16B16A16_FLOAT: return MTLPixelFormatRGBA16Float;
    case MTLB_FORMAT_R16G16B16A16_UNORM: return MTLPixelFormatRGBA16Unorm;
    case MTLB_FORMAT_R16G16B16A16_UINT: return MTLPixelFormatRGBA16Uint;
    case MTLB_FORMAT_R16G16B16A16_SNORM: return MTLPixelFormatRGBA16Snorm;
    case MTLB_FORMAT_R16G16B16A16_SINT: return MTLPixelFormatRGBA16Sint;
    case MTLB_FORMAT_R32G32_FLOAT: return MTLPixelFormatRG32Float;
    case MTLB_FORMAT_R32G32_UINT: return MTLPixelFormatRG32Uint;
    case MTLB_FORMAT_R32G32_SINT: return MTLPixelFormatRG32Sint;
    case MTLB_FORMAT_R10G10B10A2_UNORM: return MTLPixelFormatRGB10A2Unorm;
    case MTLB_FORMAT_R10G10B10A2_UINT: return MTLPixelFormatRGB10A2Uint;
    case MTLB_FORMAT_R11G11B10_FLOAT: return MTLPixelFormatRG11B10Float;
    case MTLB_FORMAT_R8G8B8A8_TYPELESS:
    case MTLB_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case MTLB_FORMAT_R8G8B8A8_UNORM_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
    case MTLB_FORMAT_R8G8B8A8_UINT: return MTLPixelFormatRGBA8Uint;
    case MTLB_FORMAT_R8G8B8A8_SNORM: return MTLPixelFormatRGBA8Snorm;
    case MTLB_FORMAT_R8G8B8A8_SINT: return MTLPixelFormatRGBA8Sint;
    case MTLB_FORMAT_R16G16_FLOAT: return MTLPixelFormatRG16Float;
    case MTLB_FORMAT_R16G16_UNORM: return MTLPixelFormatRG16Unorm;
    case MTLB_FORMAT_R16G16_UINT: return MTLPixelFormatRG16Uint;
    case MTLB_FORMAT_R16G16_SNORM: return MTLPixelFormatRG16Snorm;
    case MTLB_FORMAT_R16G16_SINT: return MTLPixelFormatRG16Sint;
    case MTLB_FORMAT_D32_FLOAT: return MTLPixelFormatDepth32Float;
    case MTLB_FORMAT_R32_FLOAT: return MTLPixelFormatR32Float;
    case MTLB_FORMAT_R32_UINT: return MTLPixelFormatR32Uint;
    case MTLB_FORMAT_R32_SINT: return MTLPixelFormatR32Sint;
    case MTLB_FORMAT_D24_UNORM_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
    case MTLB_FORMAT_R8G8_UNORM: return MTLPixelFormatRG8Unorm;
    case MTLB_FORMAT_R8G8_UINT: return MTLPixelFormatRG8Uint;
    case MTLB_FORMAT_R8G8_SNORM: return MTLPixelFormatRG8Snorm;
    case MTLB_FORMAT_R8G8_SINT: return MTLPixelFormatRG8Sint;
    case MTLB_FORMAT_R16_FLOAT: return MTLPixelFormatR16Float;
    case MTLB_FORMAT_D16_UNORM: return MTLPixelFormatDepth16Unorm;
    case MTLB_FORMAT_R16_UNORM: return MTLPixelFormatR16Unorm;
    case MTLB_FORMAT_R16_UINT: return MTLPixelFormatR16Uint;
    case MTLB_FORMAT_R16_SNORM: return MTLPixelFormatR16Snorm;
    case MTLB_FORMAT_R16_SINT: return MTLPixelFormatR16Sint;
    case MTLB_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
    case MTLB_FORMAT_R8_UINT: return MTLPixelFormatR8Uint;
    case MTLB_FORMAT_R8_SNORM: return MTLPixelFormatR8Snorm;
    case MTLB_FORMAT_R8_SINT: return MTLPixelFormatR8Sint;
    case MTLB_FORMAT_BC1_UNORM: return MTLPixelFormatBC1_RGBA;
    case MTLB_FORMAT_BC1_UNORM_SRGB: return MTLPixelFormatBC1_RGBA_sRGB;
    case MTLB_FORMAT_BC2_UNORM: return MTLPixelFormatBC2_RGBA;
    case MTLB_FORMAT_BC2_UNORM_SRGB: return MTLPixelFormatBC2_RGBA_sRGB;
    case MTLB_FORMAT_BC3_UNORM: return MTLPixelFormatBC3_RGBA;
    case MTLB_FORMAT_BC3_UNORM_SRGB: return MTLPixelFormatBC3_RGBA_sRGB;
    case MTLB_FORMAT_BC4_UNORM: return MTLPixelFormatBC4_RUnorm;
    case MTLB_FORMAT_BC4_SNORM: return MTLPixelFormatBC4_RSnorm;
    case MTLB_FORMAT_BC5_UNORM: return MTLPixelFormatBC5_RGUnorm;
    case MTLB_FORMAT_BC5_SNORM: return MTLPixelFormatBC5_RGSnorm;
    case MTLB_FORMAT_B8G8R8A8_TYPELESS:
    case MTLB_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case MTLB_FORMAT_B8G8R8A8_UNORM_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
    case MTLB_FORMAT_BC6H_UF16: return MTLPixelFormatBC6H_RGBUfloat;
    case MTLB_FORMAT_BC6H_SF16: return MTLPixelFormatBC6H_RGBFloat;
    case MTLB_FORMAT_BC7_UNORM: return MTLPixelFormatBC7_RGBAUnorm;
    case MTLB_FORMAT_BC7_UNORM_SRGB: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    default: return MTLPixelFormatInvalid;
    }
}

MTLVertexFormat to_vertex_format(uint32_t format)
{
    switch (static_cast<mtlb_format>(format)) {
    case MTLB_FORMAT_R32G32B32A32_FLOAT: return MTLVertexFormatFloat4;
    case MTLB_FORMAT_R32G32B32A32_UINT: return MTLVertexFormatUInt4;
    case MTLB_FORMAT_R32G32B32A32_SINT: return MTLVertexFormatInt4;
    case MTLB_FORMAT_R32G32B32_FLOAT: return MTLVertexFormatFloat3;
    case MTLB_FORMAT_R32G32B32_UINT: return MTLVertexFormatUInt3;
    case MTLB_FORMAT_R32G32B32_SINT: return MTLVertexFormatInt3;
    case MTLB_FORMAT_R32G32_FLOAT: return MTLVertexFormatFloat2;
    case MTLB_FORMAT_R32G32_UINT: return MTLVertexFormatUInt2;
    case MTLB_FORMAT_R32G32_SINT: return MTLVertexFormatInt2;
    case MTLB_FORMAT_R32_FLOAT: return MTLVertexFormatFloat;
    case MTLB_FORMAT_R32_UINT: return MTLVertexFormatUInt;
    case MTLB_FORMAT_R32_SINT: return MTLVertexFormatInt;
    case MTLB_FORMAT_R16G16B16A16_FLOAT: return MTLVertexFormatHalf4;
    case MTLB_FORMAT_R16G16B16A16_UNORM: return MTLVertexFormatUShort4Normalized;
    case MTLB_FORMAT_R16G16B16A16_UINT: return MTLVertexFormatUShort4;
    case MTLB_FORMAT_R16G16B16A16_SNORM: return MTLVertexFormatShort4Normalized;
    case MTLB_FORMAT_R16G16B16A16_SINT: return MTLVertexFormatShort4;
    case MTLB_FORMAT_R16G16_FLOAT: return MTLVertexFormatHalf2;
    case MTLB_FORMAT_R16G16_UNORM: return MTLVertexFormatUShort2Normalized;
    case MTLB_FORMAT_R16G16_UINT: return MTLVertexFormatUShort2;
    case MTLB_FORMAT_R16G16_SNORM: return MTLVertexFormatShort2Normalized;
    case MTLB_FORMAT_R16G16_SINT: return MTLVertexFormatShort2;
    case MTLB_FORMAT_R16_FLOAT: return MTLVertexFormatHalf;
    case MTLB_FORMAT_R16_UNORM: return MTLVertexFormatUShortNormalized;
    case MTLB_FORMAT_R16_UINT: return MTLVertexFormatUShort;
    case MTLB_FORMAT_R16_SNORM: return MTLVertexFormatShortNormalized;
    case MTLB_FORMAT_R16_SINT: return MTLVertexFormatShort;
    case MTLB_FORMAT_R10G10B10A2_UNORM: return MTLVertexFormatUInt1010102Normalized;
    case MTLB_FORMAT_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
    case MTLB_FORMAT_R8G8B8A8_UINT: return MTLVertexFormatUChar4;
    case MTLB_FORMAT_R8G8B8A8_SNORM: return MTLVertexFormatChar4Normalized;
    case MTLB_FORMAT_R8G8B8A8_SINT: return MTLVertexFormatChar4;
    case MTLB_FORMAT_B8G8R8A8_UNORM: return MTLVertexFormatUChar4Normalized_BGRA;
    case MTLB_FORMAT_R8G8_UNORM: return MTLVertexFormatUChar2Normalized;
    case MTLB_FORMAT_R8G8_UINT: return MTLVertexFormatUChar2;
    case MTLB_FORMAT_R8G8_SNORM: return MTLVertexFormatChar2Normalized;
    case MTLB_FORMAT_R8G8_SINT: return MTLVertexFormatChar2;
    case MTLB_FORMAT_R8_UNORM: return MTLVertexFormatUCharNormalized;
    case MTLB_FORMAT_R8_UINT: return MTLVertexFormatUChar;
    case MTLB_FORMAT_R8_SNORM: return MTLVertexFormatCharNormalized;
    case MTLB_FORMAT_R8_SINT: return MTLVertexFormatChar;
    default: return MTLVertexFormatInvalid;
    }
}

} // namespace mtlb
