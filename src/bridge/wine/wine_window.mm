// SPDX-License-Identifier: LGPL-2.1-or-later
// Finds the CAMetalLayer of a Wine window.
//
// Wine's macOS driver (winemac.so) keeps each top-level window in a WineWindow
// whose -hwnd is the Windows window handle; its content view can create a
// WineMetalView, a view backed by a CAMetalLayer (the path Wine's own Vulkan
// support uses). This build of winemac exports no macdrv_* functions, so the
// Objective-C objects are reached through the runtime. The PE side passes the
// top-level handle (GetAncestor(hwnd, GA_ROOT)).
//
// -newMetalViewWithDevice: returns the content view's existing metal view
// (without a new reference) when there is one, and otherwise creates it with
// the reference its "new" name promises, which the creator gives up with
// -removeFromSuperview and -release (macdrv_view_release_metal_view does).
// Several swap chains on one window therefore share one view; the first owns
// the creation reference and the view goes away with the last of them.
#import <AppKit/AppKit.h>
#import <objc/message.h>

#include <map>
#include <memory>
#include <mutex>

#include <unistd.h>

#include "bridge/metal/layer_provider.h"

namespace {

// Views handed out, by layer. Main thread only.
struct ViewEntry {
    NSView *view;
    int users;
    bool owned;  // this module holds the creation reference
};

std::map<void *, ViewEntry> &views()
{
    static std::map<void *, ViewEntry> map;
    return map;
}

// Runs on the main thread, where AppKit objects may be touched.
CAMetalLayer *find_layer(uint64_t window, id<MTLDevice> device)
{
    Class window_class = NSClassFromString(@"WineWindow");
    Class view_class = NSClassFromString(@"WineMetalView");
    if (!window_class || !view_class)
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
        bool existed = false;
        for (NSView *subview in content.subviews)
            existed = existed || [subview isKindOfClass:view_class];
        // Declared +0: ARC retains the result for the entry below, and the creation
        // reference (if any) is given up explicitly when the last user is done.
        NSView *view = ((NSView * (*)(id, SEL, id)) objc_msgSend)(content, new_view_selector, device);
        CALayer *layer = [view layer];
        if (![layer isKindOfClass:[CAMetalLayer class]])
            return nil;
        auto [it, inserted] = views().try_emplace((__bridge void *)layer, ViewEntry{view, 0, !existed});
        ++it->second.users;
        return (CAMetalLayer *)layer;
    }
    return nil;
}

// Main thread: a swap chain is done with `layer`.
void release_on_main(CAMetalLayer *layer)
{
    auto it = views().find((__bridge void *)layer);
    if (it == views().end() || --it->second.users > 0)
        return;
    NSView *view = it->second.view;
    [view removeFromSuperview];
    if (it->second.owned)
        CFRelease((__bridge CFTypeRef)view);  // the creation reference
    views().erase(it);
}

void release_layer(CAMetalLayer *layer)
{
    mtlb::run_on_main_async(^{ release_on_main(layer); });
}

// Outcome of one lookup running on the main thread, shared with the waiting thread.
struct Lookup {
    std::mutex mutex;
    bool abandoned = false;  // the caller stopped waiting
    CAMetalLayer *layer = nil;
};

CAMetalLayer *layer_for_window(uint64_t window, id<MTLDevice> device)
{
    // Wine creates the Cocoa window when the Windows window is first shown or
    // moved, which may be just after the application's CreateWindow returns. Each
    // attempt waits a bounded time for the main thread; the pause between
    // attempts grows, for a total of a few seconds.
    useconds_t pause = 2 * 1000;
    for (int attempt = 0; attempt < 40; ++attempt) {
        auto lookup = std::make_shared<Lookup>();
        mtlb::run_on_main(^{
            CAMetalLayer *layer = find_layer(window, device);
            std::lock_guard<std::mutex> lock(lookup->mutex);
            if (lookup->abandoned && layer)
                release_on_main(layer);  // nobody will use the layer found after the caller gave up
            else
                lookup->layer = layer;
        });
        std::lock_guard<std::mutex> lock(lookup->mutex);
        if (lookup->layer)
            return lookup->layer;
        lookup->abandoned = true;
        usleep(pause);
        pause = pause < 100 * 1000 ? pause * 2 : pause;
    }
    return nil;
}

__attribute__((constructor)) void install_layer_provider()
{
    mtlb::set_layer_provider(layer_for_window, release_layer);
}

} // namespace
