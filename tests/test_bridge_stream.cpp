// Command stream validation: the backend must reject malformed records
// instead of reading past them.
#include <cstring>
#include <vector>

#include "bridge/mtlb_cmd.h"
#include "test_util.h"

namespace {

// Builds a stream from raw records.
struct Stream {
    std::vector<uint8_t> bytes;

    // Appends a record of `size` bytes with the given type; `body` fills in the
    // bytes after the header.
    void add(uint32_t type, uint32_t size, const void *body = nullptr, size_t body_size = 0)
    {
        const size_t at = bytes.size();
        bytes.resize(at + mtlb_cmd_align(size));
        mtlb_cmd_header header = {type, size};
        std::memcpy(&bytes[at], &header, sizeof(header));
        if (body)
            std::memcpy(&bytes[at + sizeof(header)], body, body_size);
    }
};

} // namespace

int main()
{
    mtlb_device device = 0;
    CHECK(mtlb_device_create(&device) == MTLB_OK);
    mtlb_queue queue = 0;
    CHECK(mtlb_queue_create(device, &queue) == MTLB_OK);

    auto submit = [&](const Stream &s) { return mtlb_queue_submit(queue, s.bytes.data(), s.bytes.size()); };

    // Valid streams.
    CHECK(mtlb_queue_submit(queue, nullptr, 0) == MTLB_OK);
    Stream valid;
    valid.add(MTLB_CMD_RESET_STATE, sizeof(mtlb_cmd_reset_state));
    CHECK(submit(valid) == MTLB_OK);

    // Truncated header.
    Stream truncated;
    truncated.bytes = {1, 0, 0, 0};
    CHECK(submit(truncated) != MTLB_OK);

    // Unknown command type.
    Stream unknown;
    unknown.add(9999, sizeof(mtlb_cmd_header));
    CHECK(submit(unknown) != MTLB_OK);

    // Record smaller than its type, not a multiple of 8, or longer than the stream.
    Stream too_small;
    too_small.add(MTLB_CMD_DRAW, sizeof(mtlb_cmd_header));
    CHECK(submit(too_small) != MTLB_OK);
    Stream misaligned;
    misaligned.add(MTLB_CMD_RESET_STATE, 12);
    CHECK(submit(misaligned) != MTLB_OK);
    Stream overlong;
    overlong.add(MTLB_CMD_RESET_STATE, sizeof(mtlb_cmd_reset_state));
    overlong.bytes[4] = 64;
    CHECK(submit(overlong) != MTLB_OK);

    // Array counts larger than the record.
    Stream viewports;
    const uint32_t big_count[2] = {1000, 0};
    viewports.add(MTLB_CMD_SET_VIEWPORTS, sizeof(mtlb_cmd_set_viewports) + sizeof(mtlb_viewport), big_count, sizeof(big_count));
    CHECK(submit(viewports) != MTLB_OK);
    Stream scissors;
    scissors.add(MTLB_CMD_SET_SCISSORS, sizeof(mtlb_cmd_set_scissors), big_count, sizeof(big_count));
    CHECK(submit(scissors) != MTLB_OK);
    Stream vertex_buffers;
    const uint32_t slots[2] = {0, 4};
    vertex_buffers.add(MTLB_CMD_SET_VERTEX_BUFFERS, sizeof(mtlb_cmd_set_vertex_buffers), slots, sizeof(slots));
    CHECK(submit(vertex_buffers) != MTLB_OK);
    Stream root_args;
    const uint32_t big_size[2] = {4096, 0};
    root_args.add(MTLB_CMD_SET_GRAPHICS_ROOT_ARGS, sizeof(mtlb_cmd_set_graphics_root_args) + 16, big_size, sizeof(big_size));
    CHECK(submit(root_args) != MTLB_OK);

    // Structural errors.
    Stream end_without_begin;
    end_without_begin.add(MTLB_CMD_END_RENDER_PASS, sizeof(mtlb_cmd_end_render_pass));
    CHECK(submit(end_without_begin) != MTLB_OK);
    Stream draw_outside_pass;
    draw_outside_pass.add(MTLB_CMD_DRAW, sizeof(mtlb_cmd_draw));
    CHECK(submit(draw_outside_pass) != MTLB_OK);

    // The queue still works after rejected streams.
    CHECK(submit(valid) == MTLB_OK);

    mtlb_queue_destroy(queue);
    mtlb_device_destroy(device);
    return 0;
}
