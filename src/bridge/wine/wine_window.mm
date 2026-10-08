// Finds the CAMetalLayer of a Wine window.
//
// Wine's macOS driver (winemac.so) keeps each top-level window in a WineWindow
// whose -hwnd is the Windows window handle; its content view can create a
// WineMetalView, a view backed by a CAMetalLayer (the path Wine's own Vulkan
// support uses). This build of winemac exports no macdrv_* functions, so the
// Objective-C objects are reached through the runtime. The PE side passes the
// top-level handle (GetAncestor(hwnd, GA_ROOT)).
#import <AppKit/AppKit.h>
#import <objc/message.h>

#include <unistd.h>

#include "bridge/metal/layer_provider.h"

namespace {

// Runs on the main thread, where AppKit objects may be touched.
CAMetalLayer *find_layer(uint64_t window, id<MTLDevice> device)
{
    Class window_class = NSClassFromString(@"WineWindow");
    if (!window_class)
        return nil;
    const SEL hwnd_selector = sel_registerName("hwnd");
    const SEL new_view_selector = sel_registerName("newMetalViewWithDevice:");
    for (NSWindow *candidate in [NSApp windows]) {
        if (![candidate isKindOfClass:window_class])
            continue;
        void *hwnd = ((void *(*)(id, SEL))objc_msgSend)(candidate, hwnd_selector);
        if (reinterpret_cast<uint64_t>(hwnd) != window)
            continue;
        NSView *content = candidate.contentView;
        if (![content respondsToSelector:new_view_selector])
            return nil;
        id view = ((id(*)(id, SEL, id))objc_msgSend)(content, new_view_selector, device);
        CALayer *layer = [view layer];
        return [layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer *)layer : nil;
    }
    return nil;
}

CAMetalLayer *layer_for_window(uint64_t window, id<MTLDevice> device)
{
    // Wine creates the Cocoa window when the Windows window is first shown or
    // moved, which may be just after the application's CreateWindow returns.
    for (int attempt = 0; attempt < 200; ++attempt) {
        __block CAMetalLayer *layer = nil;
        mtlb::run_on_main(^{ layer = find_layer(window, device); });
        if (layer)
            return layer;
        usleep(10 * 1000);
    }
    return nil;
}

__attribute__((constructor)) void install_layer_provider()
{
    mtlb::set_layer_provider(layer_for_window);
}

} // namespace
