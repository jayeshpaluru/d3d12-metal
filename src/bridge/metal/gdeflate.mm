// SPDX-License-Identifier: LGPL-2.1-or-later
// The DirectStorage GDeflate decompressor's kernels (see gdeflate_kernel.msl.inc): MSL compiled at the first use.
#include "internal.h"

namespace {

const char *const kGDeflateSource =
#include "gdeflate_kernel.msl.inc"
    ;

} // namespace

namespace mtlb {

id<MTLComputePipelineState> gdeflate_kernel(Device *device, NSString *name)
{
    std::lock_guard<std::mutex> lock(device->kernels_mutex);
    const std::string key = std::string("gdeflate:") + name.UTF8String;
    if (auto it = device->kernels.find(key); it != device->kernels.end())
        return it->second;
    if (!device->gdeflate_library) {
        NSError *error = nil;
        device->gdeflate_library = [device->device newLibraryWithSource:@(kGDeflateSource) options:nil error:&error];
        if (!device->gdeflate_library) {
            fail(MTLB_ERROR_COMPILE_FAILED, std::string("GDeflate kernels: ") + error.localizedDescription.UTF8String);
            return nil;
        }
    }
    id<MTLFunction> function = [device->gdeflate_library newFunctionWithName:name];
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline = function ? [device->device newComputePipelineStateWithFunction:function error:&error] : nil;
    // The decoder maps the 32 bitstreams of a tile onto the lanes of one SIMD group.
    if (pipeline && [name isEqualToString:@"gdeflate_decode"] && pipeline.threadExecutionWidth != 32) {
        fail(MTLB_ERROR_UNSUPPORTED, "GDeflate needs a SIMD width of 32");
        return nil;
    }
    if (!pipeline) {
        fail(MTLB_ERROR_COMPILE_FAILED, std::string("GDeflate kernel ") + name.UTF8String + " unavailable");
        return nil;
    }
    device->kernels[key] = pipeline;
    return pipeline;
}

} // namespace mtlb
