// How the backend finds the CAMetalLayer of an application window.
//
// Window handles mean nothing to Metal: whoever hosts the backend supplies the
// mapping. The Wine unix module installs one that looks up the Wine window
// (wine_window.mm); a native test can install one returning a detached layer.
// Without a provider swap chain creation fails with MTLB_ERROR_UNSUPPORTED.
#pragma once

#import <QuartzCore/CAMetalLayer.h>

#include <stdint.h>

namespace mtlb {

// Returns the layer drawing into the top-level window `window`, configured for
// `device`, or nil when there is none. Called on the thread that creates the swap chain.
using LayerProvider = CAMetalLayer *(*)(uint64_t window, id<MTLDevice> device);

// Runs `block` on the main thread (CALayer and AppKit state is main-thread only).
inline void run_on_main(void (^block)(void))
{
    if ([NSThread isMainThread])
        block();
    else
        dispatch_sync(dispatch_get_main_queue(), block);
}

void set_layer_provider(LayerProvider provider);
LayerProvider layer_provider();

} // namespace mtlb

// For native tests, which have no windows: installs `provider` (see above).
extern "C" __attribute__((visibility("default"))) void mtlb_native_set_layer_provider(mtlb::LayerProvider provider);
