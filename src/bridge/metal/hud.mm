// SPDX-License-Identifier: LGPL-2.1-or-later
// The performance overlay (hud=1 / D3D12METAL_HUD=1): frame rate, frame time and GPU time per frame as a few lines of
// text in the top-left corner, drawn in the swap chain's present pass right after the back buffer.
//
// The font is original to this project: 5x7 glyphs for the characters the overlay needs, stored as bit rows and
// uploaded once as an R8 atlas (5 texels wide, 7 per glyph). One instanced quad per character, drawn twice (a dark
// offset copy first, then white), alpha-blended onto the drawable.
#include "bridge/metal/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "bridge/metal/log.h"
#include "internal.h"

namespace mtlb {

namespace {

// Glyph order in the atlas; the overlay text only uses these. Index kUnknownGlyph is a filled box.
const char kGlyphChars[] = "0123456789. :FPSMGUCT-";
constexpr unsigned kGlyphCount = sizeof(kGlyphChars) - 1 + 1;  // + the unknown glyph
constexpr unsigned kGlyphW = 5, kGlyphH = 7;

// 7 rows of 5 bits (bit 4 is the leftmost pixel).
const uint8_t kGlyphBits[kGlyphCount][kGlyphH] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},  // .
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // space
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},  // :
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // F
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // P
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // S
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},  // G
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // U
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // C
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},  // -
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F},  // unknown
};

const char *const kHudSource = R"msl(
#include <metal_stdlib>
using namespace metal;

struct HudUniforms {
    float2 drawable;   // pixels
    float2 origin;     // pixels, top-left of the first character
    float scale;       // screen pixels per glyph pixel
};

struct HudOut {
    float4 position [[position]];
    float2 texel;      // in the atlas
    float shadow;
};

vertex HudOut hud_vs(uint vid [[vertex_id]], uint iid [[instance_id]],
                     constant HudUniforms &u [[buffer(0)]], constant uchar4 *cells [[buffer(1)]])
{
    const uchar4 cell = cells[iid >> 1];   // glyph, column, row
    const bool shadow = (iid & 1) == 0;    // the dark copy goes first
    const float2 corner = float2(vid & 1, (vid >> 1) & 1);
    const float2 glyph_size = float2(5.0, 7.0);
    const float2 cell_size = float2(6.0, 8.0) * u.scale;
    float2 pos = u.origin + float2(cell.y, cell.z) * cell_size + corner * glyph_size * u.scale;
    if (shadow)
        pos += u.scale;
    HudOut out;
    out.position = float4(pos.x / u.drawable.x * 2.0 - 1.0, 1.0 - pos.y / u.drawable.y * 2.0, 0.0, 1.0);
    out.texel = float2(corner.x * 4.999, float(cell.x) * 7.0 + corner.y * 6.999);
    out.shadow = shadow ? 1.0 : 0.0;
    return out;
}

fragment float4 hud_fs(HudOut in [[stage_in]], texture2d<float> atlas [[texture(0)]])
{
    const float v = atlas.read(uint2(in.texel)).r;
    if (v < 0.5)
        discard_fragment();
    return in.shadow > 0.5 ? float4(0.0, 0.0, 0.0, 0.85) : float4(1.0, 1.0, 1.0, 1.0);
}
)msl";

struct Uniforms {
    float drawable[2];
    float origin[2];
    float scale;
    float pad[3];
};

uint8_t glyph_index(char c)
{
    for (unsigned i = 0; kGlyphChars[i]; ++i)
        if (kGlyphChars[i] == c)
            return static_cast<uint8_t>(i);
    return kGlyphCount - 1;
}

uint64_t now_ns()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

} // namespace

bool hud_wanted()
{
    const char *value = getenv("D3D12METAL_HUD");
    return value && *value && std::string(value) != "0";
}

bool hud_init(HudState &hud, id<MTLDevice> device)
{
    NSError *error = nil;
    hud.library = [device newLibraryWithSource:@(kHudSource) options:nil error:&error];
    if (!hud.library) {
        backend_log("hud shader: %s", error.localizedDescription.UTF8String);
        return false;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                   width:kGlyphW
                                                                                  height:kGlyphH * kGlyphCount
                                                                               mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    hud.atlas = [device newTextureWithDescriptor:td];
    uint8_t pixels[kGlyphW * kGlyphH * kGlyphCount];
    for (unsigned g = 0; g < kGlyphCount; ++g)
        for (unsigned y = 0; y < kGlyphH; ++y)
            for (unsigned x = 0; x < kGlyphW; ++x)
                pixels[(g * kGlyphH + y) * kGlyphW + x] = (kGlyphBits[g][y] >> (kGlyphW - 1 - x)) & 1 ? 255 : 0;
    [hud.atlas replaceRegion:MTLRegionMake2D(0, 0, kGlyphW, kGlyphH * kGlyphCount) mipmapLevel:0 withBytes:pixels bytesPerRow:kGlyphW];
    return hud.atlas != nil;
}

id<MTLRenderPipelineState> hud_make_pipeline(HudState &hud, id<MTLDevice> device, MTLPixelFormat pixel_format)
{
    if (!hud.library)
        return nil;
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [hud.library newFunctionWithName:@"hud_vs"];
    desc.fragmentFunction = [hud.library newFunctionWithName:@"hud_fs"];
    MTLRenderPipelineColorAttachmentDescriptor *color = desc.colorAttachments[0];
    color.pixelFormat = pixel_format;
    color.blendingEnabled = YES;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipeline)
        backend_log("hud pipeline: %s", error.localizedDescription.UTF8String);
    return pipeline;
}

void hud_update(HudState &hud, uint64_t presents)
{
    const uint64_t now = now_ns();
    const uint64_t gpu = g_stats[kStatGpuNanos].load(std::memory_order_relaxed);
    if (hud.last_ns == 0) {
        hud.last_ns = now;
        hud.last_presents = presents;
        hud.last_gpu_ns = gpu;
        return;
    }
    const uint64_t dt = now - hud.last_ns;
    if (dt < 500'000'000 || presents <= hud.last_presents)
        return;
    const double frames = static_cast<double>(presents - hud.last_presents);
    hud.fps = frames * 1e9 / static_cast<double>(dt);
    hud.frame_ms = static_cast<double>(dt) / frames / 1e6;
    hud.gpu_ms = static_cast<double>(gpu - hud.last_gpu_ns) / frames / 1e6;
    hud.last_ns = now;
    hud.last_presents = presents;
    hud.last_gpu_ns = gpu;
}

void hud_lines(const HudState &hud, std::string lines[3])
{
    char buf[32];
    if (hud.fps <= 0) {
        lines[0] = "-- FPS";
        lines[1] = "-- MS";
        lines[2] = "GPU -- MS";
        return;
    }
    std::snprintf(buf, sizeof buf, "%.0f FPS", hud.fps);
    lines[0] = buf;
    std::snprintf(buf, sizeof buf, "%.1f MS", hud.frame_ms);
    lines[1] = buf;
    if (hud.gpu_ms > 0)
        std::snprintf(buf, sizeof buf, "GPU %.1f MS", hud.gpu_ms);
    else
        std::snprintf(buf, sizeof buf, "GPU -- MS");
    lines[2] = buf;
}

void hud_encode(HudState &hud, id<MTLRenderCommandEncoder> encoder, id<MTLRenderPipelineState> pipeline, NSUInteger width,
                NSUInteger height)
{
    if (!pipeline || !hud.atlas)
        return;
    std::string lines[3];
    hud_lines(hud, lines);
    uchar4_cell cells[48];
    unsigned count = 0;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < lines[row].size() && count < 48; ++col)
            if (lines[row][col] != ' ')
                cells[count++] = {glyph_index(lines[row][col]), static_cast<uint8_t>(col), static_cast<uint8_t>(row), 0};
    if (count == 0)
        return;
    Uniforms u = {};
    u.drawable[0] = static_cast<float>(width);
    u.drawable[1] = static_cast<float>(height);
    u.scale = std::max(2.0f, std::floor(static_cast<float>(height) / 400.0f));
    u.origin[0] = u.origin[1] = 6.0f * u.scale;
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBytes:&u length:sizeof u atIndex:0];
    [encoder setVertexBytes:cells length:count * sizeof(uchar4_cell) atIndex:1];
    [encoder setFragmentTexture:hud.atlas atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:count * 2];
}

} // namespace mtlb
