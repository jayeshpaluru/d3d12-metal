// SPDX-License-Identifier: LGPL-2.1-or-later
// The backend must reject copy, resolve and render target records that reach outside their resources
// (Metal aborts or faults the GPU on those), and keep working afterwards.
#include <cstring>
#include <vector>

#include "bridge/mtlb_cmd.h"
#include "test_util.h"

namespace {

template <typename T>
std::vector<uint8_t> record(mtlb_cmd_type type, T body)
{
    body.header = {static_cast<uint32_t>(type), static_cast<uint32_t>(mtlb_cmd_align(sizeof(T)))};
    std::vector<uint8_t> bytes(mtlb_cmd_align(sizeof(T)));
    std::memcpy(bytes.data(), &body, sizeof(T));
    return bytes;
}

} // namespace

int main()
{
    mtlb_device device = 0;
    CHECK(mtlb_device_create(0, &device) == MTLB_OK);
    mtlb_queue queue = 0;
    CHECK(mtlb_queue_create(device, &queue) == MTLB_OK);

    auto submit = [&](const std::vector<uint8_t> &bytes) {
        const mtlb_span span = {bytes.data(), bytes.size()};
        return mtlb_queue_submit(queue, &span, 1);
    };

    mtlb_buffer a = 0, b = 0;
    CHECK(mtlb_buffer_create(device, 256, MTLB_STORAGE_SHARED, &a, nullptr) == MTLB_OK);
    CHECK(mtlb_buffer_create(device, 256, MTLB_STORAGE_SHARED, &b, nullptr) == MTLB_OK);

    // ---- buffer copies -------------------------------------------------------------------------------------
    auto copy_buffer = [&](mtlb_buffer dst, mtlb_buffer src, uint64_t dst_offset, uint64_t src_offset, uint64_t size) {
        mtlb_cmd_copy_buffer c = {};
        c.dst = dst;
        c.src = src;
        c.dst_offset = dst_offset;
        c.src_offset = src_offset;
        c.size = size;
        return submit(record(MTLB_CMD_COPY_BUFFER, c));
    };
    CHECK(copy_buffer(a, b, 0, 0, 256) == MTLB_OK);
    CHECK(copy_buffer(a, b, 128, 0, 128) == MTLB_OK);
    CHECK(copy_buffer(a, b, 256, 0, 0) == MTLB_OK);                    // empty at the end
    CHECK(copy_buffer(a, b, 0, 0, 257) != MTLB_OK);                    // too long
    CHECK(copy_buffer(a, b, 200, 0, 100) != MTLB_OK);                  // destination overrun
    CHECK(copy_buffer(a, b, 0, 200, 100) != MTLB_OK);                  // source overrun
    CHECK(copy_buffer(a, b, 257, 0, 0) != MTLB_OK);                    // offset past the end
    CHECK(copy_buffer(a, b, 0, ~0ull - 3, 8) != MTLB_OK);              // offset + size wraps
    CHECK(copy_buffer(a, a, 0, 64, 128) != MTLB_OK);                   // overlapping ranges of one buffer
    CHECK(copy_buffer(a, a, 0, 128, 128) == MTLB_OK);                  // adjacent ranges do not overlap

    // ---- texture copies ------------------------------------------------------------------------------------
    mtlb_texture_desc td = {};
    td.dimension = MTLB_TEXTURE_2D;
    td.format = MTLB_FORMAT_R8G8B8A8_UNORM;
    td.width = td.height = 16;
    td.depth_or_array_size = 2;  // two slices
    td.mip_levels = 3;
    td.sample_count = 1;
    td.usage = MTLB_TEXTURE_USAGE_SHADER_READ | MTLB_TEXTURE_USAGE_RENDER_TARGET;
    td.storage = MTLB_STORAGE_PRIVATE;
    mtlb_texture texture = 0, other = 0;
    CHECK(mtlb_texture_create(device, &td, &texture, nullptr) == MTLB_OK);
    CHECK(mtlb_texture_create(device, &td, &other, nullptr) == MTLB_OK);
    mtlb_buffer big = 0;
    CHECK(mtlb_buffer_create(device, 16 * 16 * 4 * 2, MTLB_STORAGE_SHARED, &big, nullptr) == MTLB_OK);

    auto copy_to_texture = [&](mtlb_cmd_type type, uint32_t mip, uint32_t slice, uint32_t x, uint32_t y, uint32_t w,
                               uint32_t h, uint64_t buffer_offset, uint32_t pitch, mtlb_buffer buffer) {
        mtlb_cmd_copy_texture c = {};
        c.region.texture = texture;
        c.region.buffer = buffer;
        c.region.buffer_offset = buffer_offset;
        c.region.bytes_per_row = pitch;
        c.region.bytes_per_image = pitch * h;
        c.region.mip_level = mip;
        c.region.array_slice = slice;
        c.region.x = x;
        c.region.y = y;
        c.region.width = w;
        c.region.height = h;
        c.region.depth = 1;
        return submit(record(type, c));
    };
    for (mtlb_cmd_type type : {MTLB_CMD_COPY_BUFFER_TO_TEXTURE, MTLB_CMD_COPY_TEXTURE_TO_BUFFER}) {
        CHECK(copy_to_texture(type, 0, 0, 0, 0, 16, 16, 0, 64, big) == MTLB_OK);
        CHECK(copy_to_texture(type, 2, 1, 0, 0, 4, 4, 0, 16, big) == MTLB_OK);   // last mip of the last slice
        CHECK(copy_to_texture(type, 3, 0, 0, 0, 1, 1, 0, 4, big) != MTLB_OK);    // no such mip
        CHECK(copy_to_texture(type, 0, 2, 0, 0, 16, 16, 0, 64, big) != MTLB_OK);  // no such slice
        CHECK(copy_to_texture(type, 1, 0, 0, 0, 16, 16, 0, 64, big) != MTLB_OK);  // larger than the mip (8x8)
        CHECK(copy_to_texture(type, 0, 0, 8, 8, 16, 16, 0, 64, big) != MTLB_OK);  // origin pushes it out
        CHECK(copy_to_texture(type, 0, 0, 0, 0, 16, 16, 1536, 64, big) != MTLB_OK);  // buffer too small from there
        CHECK(copy_to_texture(type, 0, 0, 0, 0, 16, 16, ~0ull - 7, 64, big) != MTLB_OK);  // offset wraps
        CHECK(copy_to_texture(type, 0, 0, 0, 0, 16, 16, 0, 32, big) != MTLB_OK);  // row pitch below a row
        CHECK(copy_to_texture(type, 0, 0, 0, 0, 16, 16, 0, 64, a) != MTLB_OK);    // 256-byte buffer
    }

    auto copy_texture = [&](uint32_t dst_mip, uint32_t dst_slice, uint32_t src_mip, uint32_t src_slice, uint32_t x,
                            uint32_t w, uint32_t whole) {
        mtlb_cmd_copy_texture_texture c = {};
        c.dst = other;
        c.src = texture;
        c.dst_mip = dst_mip;
        c.dst_slice = dst_slice;
        c.src_mip = src_mip;
        c.src_slice = src_slice;
        c.dst_x = x;
        c.width = w;
        c.height = w;
        c.depth = 1;
        c.whole = whole;
        return submit(record(MTLB_CMD_COPY_TEXTURE_TEXTURE, c));
    };
    CHECK(copy_texture(0, 0, 0, 0, 0, 16, 0) == MTLB_OK);
    CHECK(copy_texture(2, 1, 2, 0, 0, 4, 0) == MTLB_OK);
    CHECK(copy_texture(0, 0, 0, 0, 0, 0, 1) == MTLB_OK);                // whole resource
    CHECK(copy_texture(3, 0, 0, 0, 0, 4, 0) != MTLB_OK);                // no such destination mip
    CHECK(copy_texture(0, 0, 0, 2, 0, 4, 0) != MTLB_OK);                // no such source slice
    CHECK(copy_texture(0, 0, 0, 0, 8, 16, 0) != MTLB_OK);               // destination box out of range
    CHECK(copy_texture(0, 0, 1, 0, 0, 16, 0) != MTLB_OK);               // source mip 1 is 8x8
    mtlb_texture_desc small = td;
    small.mip_levels = 1;
    mtlb_texture small_texture = 0;
    CHECK(mtlb_texture_create(device, &small, &small_texture, nullptr) == MTLB_OK);
    {
        mtlb_cmd_copy_texture_texture c = {};
        c.dst = small_texture;
        c.src = texture;
        c.whole = 1;
        CHECK(submit(record(MTLB_CMD_COPY_TEXTURE_TEXTURE, c)) != MTLB_OK);  // different mip counts
    }

    // ---- render target views and resolves ------------------------------------------------------------------
    struct ClearBody {
        mtlb_render_target target;
        float color[4];
    };
    auto clear = [&](uint32_t mip, uint32_t slice) {
        mtlb_cmd_clear_rtv c = {};
        c.target = {texture, 0, mip, slice, 0};
        return submit(record(MTLB_CMD_CLEAR_RTV, c));
    };
    CHECK(clear(0, 0) == MTLB_OK);
    CHECK(clear(2, 1) == MTLB_OK);
    CHECK(clear(3, 0) != MTLB_OK);
    CHECK(clear(0, 2) != MTLB_OK);
    {
        std::vector<uint8_t> bytes(sizeof(mtlb_cmd_set_render_targets));
        mtlb_cmd_set_render_targets s = {};
        s.header = {MTLB_CMD_SET_RENDER_TARGETS, static_cast<uint32_t>(bytes.size())};
        s.count = 0;
        s.depth = {texture, 0, 7, 0, 0};  // a depth-stencil view of a mip that does not exist
        std::memcpy(bytes.data(), &s, sizeof(s));
        CHECK(submit(bytes) != MTLB_OK);
    }

    mtlb_texture_desc ms = td;
    ms.depth_or_array_size = 1;
    ms.mip_levels = 1;
    ms.sample_count = 4;
    mtlb_texture ms_texture = 0, resolved = 0;
    CHECK(mtlb_texture_create(device, &ms, &ms_texture, nullptr) == MTLB_OK);
    mtlb_texture_desc single = td;
    single.depth_or_array_size = 1;
    single.mip_levels = 2;
    CHECK(mtlb_texture_create(device, &single, &resolved, nullptr) == MTLB_OK);
    auto resolve = [&](uint32_t dst_mip, uint32_t dst_slice, uint32_t src_slice) {
        mtlb_cmd_resolve r = {};
        r.dst = resolved;
        r.src = ms_texture;
        r.dst_mip = dst_mip;
        r.dst_slice = dst_slice;
        r.src_slice = src_slice;
        r.format = MTLB_FORMAT_R8G8B8A8_UNORM;
        return submit(record(MTLB_CMD_RESOLVE, r));
    };
    CHECK(resolve(0, 0, 0) == MTLB_OK);
    CHECK(resolve(2, 0, 0) != MTLB_OK);  // no such mip
    CHECK(resolve(0, 1, 0) != MTLB_OK);  // no such slice
    CHECK(resolve(0, 0, 1) != MTLB_OK);
    CHECK(resolve(1, 0, 0) != MTLB_OK);  // mip 1 is 8x8, the source is 16x16

    // ---- block-compressed mips smaller than a block ------------------------------------------------------------
    // D3D12 copies a whole 2x2 or 1x1 mip of a BC texture with a box rounded up to the block size (4x4).
    for (mtlb_format format : {MTLB_FORMAT_BC1_UNORM, MTLB_FORMAT_BC7_UNORM}) {
        mtlb_texture_desc bc = {};
        bc.dimension = MTLB_TEXTURE_2D;
        bc.format = format;
        bc.width = bc.height = 4;  // mips: 4x4, 2x2, 1x1
        bc.depth_or_array_size = 1;
        bc.mip_levels = 3;
        bc.sample_count = 1;
        bc.usage = MTLB_TEXTURE_USAGE_SHADER_READ;
        bc.storage = MTLB_STORAGE_PRIVATE;
        mtlb_texture bc_a = 0, bc_b = 0;
        CHECK(mtlb_texture_create(device, &bc, &bc_a, nullptr) == MTLB_OK);
        CHECK(mtlb_texture_create(device, &bc, &bc_b, nullptr) == MTLB_OK);
        const uint32_t block_bytes = format == MTLB_FORMAT_BC1_UNORM ? 8 : 16;
        for (uint32_t mip : {1u, 2u}) {
            for (mtlb_cmd_type type : {MTLB_CMD_COPY_BUFFER_TO_TEXTURE, MTLB_CMD_COPY_TEXTURE_TO_BUFFER}) {
                mtlb_cmd_copy_texture c = {};
                c.region.texture = bc_a;
                c.region.buffer = big;
                c.region.bytes_per_row = block_bytes;
                c.region.bytes_per_image = block_bytes;
                c.region.mip_level = mip;
                c.region.width = c.region.height = 4;  // rounded up to the block: covers the whole 2x2 / 1x1 mip
                c.region.depth = 1;
                CHECK(submit(record(type, c)) == MTLB_OK);
                c.region.width = 8;                      // beyond the rounding
                CHECK(submit(record(type, c)) != MTLB_OK);
                c.region.width = 4;
                c.region.x = 4;                          // starting past the mip
                CHECK(submit(record(type, c)) != MTLB_OK);
            }
            mtlb_cmd_copy_texture_texture t = {};
            t.dst = bc_b;
            t.src = bc_a;
            t.dst_mip = t.src_mip = mip;
            t.width = t.height = 4;
            t.depth = 1;
            CHECK(submit(record(MTLB_CMD_COPY_TEXTURE_TEXTURE, t)) == MTLB_OK);
            t.width = 8;
            CHECK(submit(record(MTLB_CMD_COPY_TEXTURE_TEXTURE, t)) != MTLB_OK);
        }
        mtlb_texture_destroy(bc_b);
        mtlb_texture_destroy(bc_a);
    }

    // The queue is still usable.
    CHECK(clear(0, 0) == MTLB_OK);

    mtlb_texture_destroy(resolved);
    mtlb_texture_destroy(ms_texture);
    mtlb_texture_destroy(small_texture);
    mtlb_texture_destroy(other);
    mtlb_texture_destroy(texture);
    mtlb_buffer_destroy(big);
    mtlb_buffer_destroy(b);
    mtlb_buffer_destroy(a);
    mtlb_queue_destroy(queue);
    mtlb_device_destroy(device);
    std::printf("test_backend_ranges: OK\n");
    return 0;
}
