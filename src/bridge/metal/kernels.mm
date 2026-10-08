// Compute kernels the backend runs itself (UAV clears, query resolves, indirect argument translation).
// They are written in MSL, compiled when first needed and cached per device.
#include "internal.h"

namespace {

const char *const kKernelSource = R"msl(
#include <metal_stdlib>
using namespace metal;

// Fills `params.x` elements of `params.y` bytes with the pattern, starting `base` bytes into `dst`.
kernel void clear_buffer(device uchar *dst [[buffer(0)]], constant uchar *pattern [[buffer(1)]],
                         constant uint2 &params [[buffer(2)]], constant ulong &base [[buffer(3)]],
                         uint i [[thread_position_in_grid]])
{
    if (i >= params.x)
        return;
    device uchar *element = dst + base + ulong(i) * params.y;
    for (uint b = 0; b < params.y; ++b)
        element[b] = pattern[b];
}

// Texture clears: `r` is the region (x, y, width, height) of the view's level; the grid's z covers the
// slices of an array view or the depth of a 3D view.
#define CLEAR_KERNELS(SUFFIX, T)                                                                                      \
kernel void clear2d_##SUFFIX(texture2d<T, access::write> t [[texture(0)]], constant T##4 &v [[buffer(0)]],            \
                             constant uint4 &r [[buffer(1)]], uint2 tid [[thread_position_in_grid]])                  \
{                                                                                                                     \
    if (tid.x >= r.z || tid.y >= r.w)                                                                                 \
        return;                                                                                                       \
    t.write(v, tid + r.xy);                                                                                           \
}                                                                                                                     \
kernel void clear2da_##SUFFIX(texture2d_array<T, access::write> t [[texture(0)]], constant T##4 &v [[buffer(0)]],     \
                              constant uint4 &r [[buffer(1)]], uint3 tid [[thread_position_in_grid]])                 \
{                                                                                                                     \
    if (tid.x >= r.z || tid.y >= r.w)                                                                                 \
        return;                                                                                                       \
    t.write(v, tid.xy + r.xy, tid.z);                                                                                 \
}                                                                                                                     \
kernel void clear3d_##SUFFIX(texture3d<T, access::write> t [[texture(0)]], constant T##4 &v [[buffer(0)]],            \
                             constant uint4 &r [[buffer(1)]], uint3 tid [[thread_position_in_grid]])                 \
{                                                                                                                     \
    if (tid.x >= r.z || tid.y >= r.w)                                                                                 \
        return;                                                                                                       \
    t.write(v, uint3(tid.xy + r.xy, tid.z));                                                                          \
}

CLEAR_KERNELS(f, float)
CLEAR_KERNELS(u, uint)
CLEAR_KERNELS(i, int)
)msl";

} // namespace

namespace mtlb {

id<MTLComputePipelineState> internal_kernel(Device *device, NSString *name)
{
    std::lock_guard<std::mutex> lock(device->kernels_mutex);
    if (!device->kernel_library) {
        NSError *error = nil;
        device->kernel_library = [device->device newLibraryWithSource:@(kKernelSource) options:nil error:&error];
        if (!device->kernel_library) {
            fail(MTLB_ERROR_COMPILE_FAILED, std::string("internal kernels: ") + error.localizedDescription.UTF8String);
            return nil;
        }
    }
    __strong id<MTLComputePipelineState> &pipeline = device->kernels[name.UTF8String];
    if (!pipeline) {
        id<MTLFunction> function = [device->kernel_library newFunctionWithName:name];
        NSError *error = nil;
        if (function)
            pipeline = [device->device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            device->kernels.erase(name.UTF8String);
            fail(MTLB_ERROR_COMPILE_FAILED, std::string("internal kernel ") + name.UTF8String + " unavailable");
            return nil;
        }
    }
    return pipeline;
}

} // namespace mtlb
