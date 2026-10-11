// SPDX-License-Identifier: LGPL-2.1-or-later
// Checks GDeflate calls dumped by D3D12METAL_GDEFLATE_DUMP=<dir> against the reference decoder.
// Build and run with tools/gdeflate_verify.sh <dir>. A dump is "GDV2", u32 stream count, u64 output address, u64 output
// extent, then per stream u32 output offset, u32 input and output sizes followed by the stream's compressed bytes and the
// layer's output (sizes 0: the stream did not fit the dumped part of the buffers). Also prints the highest byte written
// per output address.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

extern "C" {
#include "libdeflate.h"
}

namespace {

uint32_t dword(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Decodes one stream (header, tile table, tiles) and compares it with `output`; prints the first difference.
bool check_stream(libdeflate_gdeflate_decompressor *dec, const char *name, uint32_t index, const uint8_t *input, size_t in_size,
                  const uint8_t *output, size_t out_size)
{
    const uint32_t tiles = dword(input) >> 16, last = (dword(input + 4) >> 2) & 0x3ffff;
    const uint8_t *table = input + 8;
    const uint8_t *data = table + 4ull * tiles;
    std::vector<uint8_t> ref(65536);
    for (uint32_t t = 0; t < tiles; ++t) {
        const size_t start = t == 0 ? 0 : dword(table + 4 * t);
        const size_t end = t + 1 < tiles ? dword(table + 4 * (t + 1)) : start + dword(table);
        const size_t want = t + 1 == tiles && last ? last : 65536;
        if (data + end > input + in_size || 65536ull * t + want > out_size) {
            std::printf("%s stream %u tile %u/%u: outside the dumped bytes\n", name, index, t, tiles);
            return false;
        }
        libdeflate_gdeflate_in_page page{data + start, end - start};
        size_t actual = 0;
        const auto r = libdeflate_gdeflate_decompress(dec, &page, 1, ref.data(), want, &actual);
        const uint8_t *got = output + 65536ull * t;
        if (r != LIBDEFLATE_SUCCESS || actual != want) {
            std::printf("%s stream %u tile %u/%u: reference decode failed (%d)\n", name, index, t, tiles, int(r));
            return false;
        }
        if (std::memcmp(ref.data(), got, want) != 0) {
            size_t first = 0, diff = 0;
            while (first < want && ref[first] == got[first])
                ++first;
            for (size_t k = 0; k < want; ++k)
                diff += ref[k] != got[k];
            std::printf("%s stream %u tile %u/%u (%zu compressed bytes): output differs, first at %zu, %zu of %zu bytes differ\n", name,
                        index, t, tiles, end - start, first, diff, want);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    libdeflate_gdeflate_decompressor *dec = libdeflate_alloc_gdeflate_decompressor();
    long bad_streams = 0, skipped = 0, streams = 0;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> highest;  // {output address, extent} -> highest end written
    for (int a = 1; a < argc; ++a) {
        FILE *f = std::fopen(argv[a], "rb");
        if (!f)
            continue;
        std::vector<uint8_t> file;
        uint8_t buffer[1 << 16];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
            file.insert(file.end(), buffer, buffer + n);
        std::fclose(f);
        if (file.size() < 8 || std::memcmp(file.data(), "GDV2", 4) != 0) {
            std::printf("%s: not a GDV2 dump\n", argv[a]);
            continue;
        }
        const uint32_t count = dword(&file[4]);
        uint64_t address = 0, extent = 0;
        std::memcpy(&address, &file[8], 8);
        std::memcpy(&extent, &file[16], 8);
        size_t at = 24;
        for (uint32_t i = 0; i < count && at + 12 <= file.size(); ++i) {
            const uint32_t out_off = dword(&file[at]), in_size = dword(&file[at + 4]), out_size = dword(&file[at + 8]);
            at += 12;
            uint64_t &top = highest[{address, extent}];
            top = std::max<uint64_t>(top, uint64_t(out_off) + out_size);
            if (at + in_size + out_size > file.size())
                break;
            ++streams;
            if (in_size == 0) {
                ++skipped;
                std::printf("%s stream %u: not in the dump (header or data outside the dumped bytes)\n", argv[a], i);
            } else if (!check_stream(dec, argv[a], i, &file[at], in_size, &file[at + in_size], out_size)) {
                ++bad_streams;
            }
            at += in_size + out_size;
        }
    }
    for (const auto &[key, top] : highest)
        std::printf("output %llx (extent %llu MiB): highest byte written at %llu MiB\n", (unsigned long long)key.first,
                    (unsigned long long)(key.second >> 20), (unsigned long long)(top >> 20));
    std::printf("%ld streams, %ld mismatching, %ld not dumped\n", streams, bad_streams, skipped);
    return bad_streams != 0;
}
