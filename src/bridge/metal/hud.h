// SPDX-License-Identifier: LGPL-2.1-or-later
// The performance overlay drawn by the swap chain's present pass (see hud.mm).
#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <string>

namespace mtlb {

struct uchar4_cell {
    uint8_t glyph, column, row, pad;
};

struct HudState {
    id<MTLLibrary> library = nil;
    id<MTLTexture> atlas = nil;
    // Measured over the last window of about half a second.
    uint64_t last_ns = 0, last_presents = 0, last_gpu_ns = 0;
    double fps = 0, frame_ms = 0, gpu_ms = 0;
};

// True when the overlay is switched on (D3D12METAL_HUD / hud=1 in d3d12metal.conf).
bool hud_wanted();
// Compiles the shader and uploads the font; false (and a log line) on failure.
bool hud_init(HudState &hud, id<MTLDevice> device);
id<MTLRenderPipelineState> hud_make_pipeline(HudState &hud, id<MTLDevice> device, MTLPixelFormat pixel_format);
// Called once per present: refreshes the numbers when the window has elapsed.
void hud_update(HudState &hud, uint64_t presents);
// The text of the three lines.
void hud_lines(const HudState &hud, std::string lines[3]);
// Draws the overlay into an open render encoder whose attachment is `width` x `height`.
void hud_encode(HudState &hud, id<MTLRenderCommandEncoder> encoder, id<MTLRenderPipelineState> pipeline, NSUInteger width,
                NSUInteger height);

} // namespace mtlb
