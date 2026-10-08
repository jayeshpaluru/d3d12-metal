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

// Called when the swap chain that obtained `layer` from the provider is destroyed.
// May run on any thread; must not block on the main thread.
using LayerReleaser = void (*)(CAMetalLayer *layer);

// Runs `block` on the main thread (CALayer and AppKit state is main-thread only)
// and waits up to `timeout_ms` for it. Never blocks unboundedly: the main thread
// may itself be waiting for the calling thread (winemac's event handling does).
// Returns false if the block had not finished in time; it still runs later, so it
// must only touch state that outlives the caller (captured by value or __block).
inline bool run_on_main(void (^block)(void), uint32_t timeout_ms = 250)
{
    if ([NSThread isMainThread]) {
        block();
        return true;
    }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        block();
        dispatch_semaphore_signal(done);
    });
    return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t(timeout_ms) * NSEC_PER_MSEC)) == 0;
}

// Runs `block` on the main thread without waiting.
inline void run_on_main_async(void (^block)(void))
{
    if ([NSThread isMainThread])
        block();
    else
        dispatch_async(dispatch_get_main_queue(), block);
}

void set_layer_provider(LayerProvider provider, LayerReleaser releaser = nullptr);
LayerProvider layer_provider();
LayerReleaser layer_releaser();

} // namespace mtlb

// For native tests, which have no windows: installs `provider` (see above).
extern "C" __attribute__((visibility("default"))) void mtlb_native_set_layer_provider(mtlb::LayerProvider provider);
