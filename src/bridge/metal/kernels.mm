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

// WriteBufferImmediate: stores a 32- or 64-bit value `size` bytes wide at `base` bytes into `dst`.
kernel void write_immediate(device uchar *dst [[buffer(0)]], constant ulong &value [[buffer(1)]],
                            constant ulong2 &where [[buffer(2)]])
{
    device uchar *at = dst + where.x;
    if (where.y == 8)
        *reinterpret_cast<device ulong *>(at) = value;
    else
        *reinterpret_cast<device uint *>(at) = uint(value);
}

// A timestamp query takes its sample at the end of a compute pass, which needs a dispatch to exist.
kernel void noop_kernel() {}

// ExecuteIndirect: turns the application's commands into one record per command for the draws or dispatches the
// CPU encodes: a copy of the current root arguments (and vertex buffer table) with the command's changes applied,
// and its action arguments, zeroed for commands past the count.
struct IndirectArg { uint type; uint dst_offset; uint size; uint src_offset; };
struct IndirectParams {
    uint action;          // 1 draw, 2 indexed draw, 3 dispatch
    uint max_count;
    uint stride;
    uint action_src;
    uint num_args;
    uint root_size;       // bytes of root arguments in a record
    uint vb_offset;       // offset of the vertex buffer table in a record
    uint action_offset;   // offset of the action arguments in a record
    uint record_size;
    uint has_count;
    uint2 pad;
};

kernel void translate_indirect(device const uchar *app [[buffer(0)]], constant IndirectArg *args [[buffer(1)]],
                               constant uchar *root_template [[buffer(2)]], constant uchar *vb_template [[buffer(3)]],
                               device uchar *out [[buffer(4)]], constant IndirectParams &p [[buffer(5)]],
                               constant ulong2 &base [[buffer(6)]], device const uchar *count_buffer [[buffer(7)]],
                               uint i [[thread_position_in_grid]])
{
    if (i >= p.max_count)
        return;
    device uchar *record = out + ulong(i) * p.record_size;
    for (uint b = 0; b < p.root_size; ++b)
        record[b] = root_template[b];
    for (uint b = 0; b < 496; ++b)
        record[p.vb_offset + b] = vb_template[b];
    device const uchar *command = app + base.x + ulong(i) * p.stride;
    const bool live = !p.has_count || i < *reinterpret_cast<device const uint *>(count_buffer + base.y);

    for (uint a = 0; a < p.num_args; ++a) {
        const IndirectArg arg = args[a];
        switch (arg.type) {
        case 1:
            for (uint k = 0; k < arg.size / 4; ++k)
                *reinterpret_cast<device uint *>(record + arg.dst_offset + 4 * k) =
                    *reinterpret_cast<device const uint *>(command + arg.src_offset + 4 * k);
            break;
        case 2:
            for (uint k = 0; k < 2; ++k)
                *reinterpret_cast<device uint *>(record + arg.dst_offset + 4 * k) =
                    *reinterpret_cast<device const uint *>(command + arg.src_offset + 4 * k);
            break;
        case 3:
            for (uint k = 0; k < 4; ++k)
                *reinterpret_cast<device uint *>(record + p.vb_offset + arg.dst_offset * 16 + 4 * k) =
                    *reinterpret_cast<device const uint *>(command + arg.src_offset + 4 * k);
            break;
        case 4:
            *reinterpret_cast<device uint *>(record + arg.dst_offset) = i;
            break;
        }
    }
    const uint action_words = p.action == 1 ? 4 : p.action == 2 ? 5 : 3;
    for (uint k = 0; k < 5; ++k) {
        *reinterpret_cast<device uint *>(record + p.action_offset + 4 * k) =
            (live && k < action_words) ? *reinterpret_cast<device const uint *>(command + p.action_src + 4 * k) : 0u;
    }
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
