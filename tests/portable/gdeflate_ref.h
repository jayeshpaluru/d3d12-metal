// Makes GDeflate tile streams with the reference compressor (NVIDIA's libdeflate fork in
// third_party/libdeflate-gdeflate). The stream layout follows Microsoft's DirectStorage GDeflate reference
// (github.com/microsoft/DirectStorage, GDeflate/GDeflate/GDeflateCompress.cpp, Apache License 2.0,
// Copyright (c) 2020-2022 NVIDIA CORPORATION & AFFILIATES, Copyright (c) Microsoft Corporation):
//   8-byte header {id 4, ~id, tile count (16 bits), tile size index 1 (2 bits), last tile size (18 bits, 0 = full)},
//   a dword per tile (the first holds the compressed size of the last tile, the others the offsets of the tiles'
//   data), then the tiles' data back to back. Tiles are 64 KiB of the original.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "libdeflate.h"
}

namespace gdeflate_ref {

constexpr size_t kTile = 64 * 1024;

// Compresses `size` (at least 1) bytes at `level` 0 (stored blocks only) to 12 into a tile stream.
inline std::vector<uint8_t> compress(const uint8_t *data, size_t size, int level)
{
    const size_t tiles = (size + kTile - 1) / kTile;
    libdeflate_gdeflate_compressor *compressor = libdeflate_alloc_gdeflate_compressor(level);
    size_t pages = 0;
    const size_t bound = libdeflate_gdeflate_compress_bound(compressor, kTile, &pages);
    std::vector<std::vector<uint8_t>> parts(tiles);
    for (size_t t = 0; t < tiles; ++t) {
        const size_t n = std::min(kTile, size - t * kTile);
        parts[t].resize(bound);
        libdeflate_gdeflate_out_page page{parts[t].data(), bound};
        if (!libdeflate_gdeflate_compress(compressor, data + t * kTile, n, &page, 1))
            std::abort();
        parts[t].resize(page.nbytes);
    }
    libdeflate_free_gdeflate_compressor(compressor);

    std::vector<uint32_t> table(tiles);
    size_t total = 0;
    for (size_t t = 0; t < tiles; ++t) {
        table[t] = static_cast<uint32_t>(total);
        total += parts[t].size();
    }
    table[0] = static_cast<uint32_t>(parts.back().size());
    const size_t last = size - (tiles - 1) * kTile;
    const uint32_t word0 = 4u | (uint32_t(4 ^ 0xff) << 8) | (uint32_t(tiles) << 16);
    const uint32_t word1 = 1u | (uint32_t(last == kTile ? 0 : last) << 2);
    std::vector<uint8_t> stream(8 + 4 * tiles + total);
    std::memcpy(stream.data(), &word0, 4);
    std::memcpy(stream.data() + 4, &word1, 4);
    std::memcpy(stream.data() + 8, table.data(), 4 * tiles);
    size_t at = 8 + 4 * tiles;
    for (const auto &part : parts) {
        std::memcpy(stream.data() + at, part.data(), part.size());
        at += part.size();
    }
    return stream;
}

} // namespace gdeflate_ref
