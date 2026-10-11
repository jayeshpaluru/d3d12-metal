// SPDX-License-Identifier: LGPL-2.1-or-later
// Objective-C side of test_swapchain: the layer that stands in for a window, and
// reading a PNG. Kept apart because Objective-C's BOOL clashes with the D3D headers.
#import <ImageIO/ImageIO.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstring>

#include "bridge/metal/layer_provider.h"
#include "swapchain_support.h"

namespace {

CAMetalLayer *provide_layer(uint64_t window, id<MTLDevice>)
{
    static CAMetalLayer *layer = [CAMetalLayer layer];
    return window == 1 ? layer : nil;
}

} // namespace

void test_install_layer_provider()
{
    mtlb_native_set_layer_provider(provide_layer);
}

bool test_png_pixel(const char *path, size_t x, size_t y, uint8_t rgb[3])
{
    NSURL *url = [NSURL fileURLWithPath:@(path)];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!source)
        return false;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!image)
        return false;
    uint8_t rgba[4] = {};
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(rgba, 1, 1, 8, 4, space, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(context,
                       CGRectMake(-double(x), -double(CGImageGetHeight(image) - 1 - y), double(CGImageGetWidth(image)),
                                  double(CGImageGetHeight(image))),
                       image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    std::memcpy(rgb, rgba, 3);
    return true;
}
