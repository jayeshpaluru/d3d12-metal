// Appending command records to a stream.
#pragma once

#include <cstdint>
#include <vector>

#include "bridge/mtlb_cmd.h"

namespace d3d12m {

// Appends a zero-filled record of type T (plus `extra_bytes` of trailing array) and returns it.
template <typename T>
T *append_record(std::vector<uint8_t> &stream, mtlb_cmd_type type, size_t extra_bytes = 0)
{
    const size_t size = mtlb_cmd_align(static_cast<uint32_t>(sizeof(T) + extra_bytes));
    const size_t at = stream.size();
    stream.resize(at + size);
    auto *cmd = reinterpret_cast<T *>(stream.data() + at);
    cmd->header = {static_cast<uint32_t>(type), static_cast<uint32_t>(size)};
    return cmd;
}

} // namespace d3d12m
