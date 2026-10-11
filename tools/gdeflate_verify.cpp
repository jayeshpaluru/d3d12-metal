// Checks GDeflate calls dumped by D3D12METAL_GDEFLATE_DUMP=<dir> against the reference decoder.
// Build and run with tools/gdeflate_verify.sh <dir>.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "libdeflate.h"
}

int main(int argc, char **argv)
{
    libdeflate_gdeflate_decompressor *dec = libdeflate_alloc_gdeflate_decompressor();
    long bad_streams = 0, streams = 0;
    for (int a = 1; a < argc; ++a) {
        FILE *f = fopen(argv[a], "rb");
        if (!f)
            continue;
        uint64_t sizes[3];
        if (fread(sizes, 8, 3, f) != 3) {
            fclose(f);
            continue;
        }
        std::vector<uint8_t> control(sizes[0]), input(sizes[1]), output(sizes[2]);
        const bool ok = fread(control.data(), 1, control.size(), f) == control.size()
                        && fread(input.data(), 1, input.size(), f) == input.size()
                        && fread(output.data(), 1, output.size(), f) == output.size();
        fclose(f);
        if (!ok)
            continue;
        uint32_t n = 0;
        memcpy(&n, control.data(), 4);
        for (uint32_t i = 0; i < n && 4 + 8 * (i + 1) <= control.size(); ++i) {
            uint32_t in_off, out_off, w0, w1;
            memcpy(&in_off, &control[4 + 8 * i], 4);
            memcpy(&out_off, &control[8 + 8 * i], 4);
            ++streams;
            if (in_off + 8ull > input.size()) {
                printf("%s stream %u: header outside input\n", argv[a], i);
                continue;
            }
            memcpy(&w0, &input[in_off], 4);
            memcpy(&w1, &input[in_off + 4], 4);
            const uint32_t tiles = w0 >> 16, last = (w1 >> 2) & 0x3ffff;
            if (!tiles || in_off + 8ull + 4ull * tiles > input.size()) {
                printf("%s stream %u: bad header %08x %08x\n", argv[a], i, w0, w1);
                continue;
            }
            std::vector<uint32_t> table(tiles);
            memcpy(table.data(), &input[in_off + 8], 4 * tiles);
            const size_t data = in_off + 8 + 4ull * tiles;
            bool bad = false;
            for (uint32_t t = 0; t < tiles && !bad; ++t) {
                const size_t start = t == 0 ? 0 : table[t];
                const size_t end = t + 1 < tiles ? table[t + 1] : start + table[0];
                const size_t want = t + 1 == tiles && last ? last : 65536;
                if (data + end > input.size()) {
                    printf("%s stream %u tile %u/%u: compressed data outside input\n", argv[a], i, t, tiles);
                    bad = true;
                    break;
                }
                std::vector<uint8_t> ref(65536);
                libdeflate_gdeflate_in_page page{&input[data + start], end - start};
                size_t actual = 0;
                const auto r = libdeflate_gdeflate_decompress(dec, &page, 1, ref.data(), want, &actual);
                const uint64_t at = out_off + 65536ull * t;
                if (r != LIBDEFLATE_SUCCESS || actual != want) {
                    printf("%s stream %u tile %u/%u: reference decode failed (%d)\n", argv[a], i, t, tiles, int(r));
                    bad = true;
                } else if (at + want > output.size() || memcmp(ref.data(), &output[at], want)) {
                    size_t first = 0, diff = 0;
                    while (first < want && at + first < output.size() && ref[first] == output[at + first])
                        ++first;
                    for (size_t k = 0; k < want && at + k < output.size(); ++k)
                        diff += ref[k] != output[at + k];
                    printf("%s stream %u tile %u/%u (%zu compressed bytes): GPU output differs, first at %zu, %zu of %zu bytes differ\n",
                           argv[a], i, t, tiles, end - start, first, diff, want);
                    bad = true;
                }
            }
            bad_streams += bad;
        }
    }
    printf("%ld streams, %ld mismatching\n", streams, bad_streams);
    return bad_streams != 0;
}
