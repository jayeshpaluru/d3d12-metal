// GDeflate decompression kernels (MSL), included as a raw string by gdeflate.mm and by the standalone tools that
// compile it. See THIRD_PARTY_NOTICES.md: the decoding algorithm (a SIMD group of 32 lanes decodes the 32
// interleaved bitstreams of one 64 KiB tile, one Huffman symbol per lane and round) is a port of the reference
// HLSL decompressor of github.com/microsoft/DirectStorage (GDeflate/shaders/GDeflate.hlsl, Apache License 2.0,
// Copyright (c) 2020-2022 NVIDIA CORPORATION & AFFILIATES, Copyright (c) Microsoft Corporation). Addresses are
// bounds-checked against the buffer sizes, so damaged streams cannot read or write outside the buffers.
R"msl(
#include <metal_stdlib>
using namespace metal;

constant uint kTileSize = 65536;
constant uint kStreamHeaderSize = 8;
constant uint kRowWidth = 4096;   // threadgroups per row of the 2D decode grid

// Everything the two kernels need. Addresses are GPU virtual addresses; sizes are the bytes the buffers hold from
// there (clamped by the caller to the buffers' ends).
struct GDParams {
    ulong input, output, control, scratch;
    ulong input_size, output_size, control_size, scratch_size;
    uint stream_count;   // streams to decompress (at most the control buffer's own count)
    uint max_streams;    // streams the scratch buffer has room for
    uint pad0, pad1;
};

// Scratch layout: {total tiles, 1, 1 (indirect dispatch), stream count, prefix[0..n]} of dwords, where prefix[i] is
// the number of tiles of streams before i. The decode grid is 2D (kRowWidth threadgroups per row).
constant uint kScratchPrefix = 4;

// Control buffer: dword stream count, then {input offset, output offset} per stream.
inline uint ctl_count(constant GDParams &p)
{
    const uint declared = *reinterpret_cast<device const uint *>(p.control);
    const uint fit = p.control_size >= 4 ? uint((p.control_size - 4) / 8) : 0u;
    return min(min(declared, fit), min(p.stream_count, p.max_streams));
}

// A dword of the input at any byte offset (tile streams need not be aligned); zero past the end.
inline uint load_dword(constant GDParams &p, uint offset)
{
    if (ulong(offset) + 4 > p.input_size)
        return 0;
    device const uchar *b = reinterpret_cast<device const uchar *>(p.input) + offset;
    if (((p.input + offset) & 3) == 0)
        return *reinterpret_cast<device const uint *>(b);
    return uint(b[0]) | (uint(b[1]) << 8) | (uint(b[2]) << 16) | (uint(b[3]) << 24);
}

inline bool stream_valid(constant GDParams &p, uint input_offset, thread uint &tiles)
{
    tiles = 0;
    if (p.input_size < 8 || input_offset > p.input_size - 8)
        return false;
    const uint w0 = load_dword(p, input_offset);
    // id 4 (GDeflate) and its complement
    if ((w0 & 0xffu) != 4u || ((w0 >> 8) & 0xffu) != 0xfbu)
        return false;
    tiles = w0 >> 16;
    // the tile offset table must lie inside the input
    if (ulong(input_offset) + kStreamHeaderSize + ulong(tiles) * 4 > p.input_size)
        tiles = 0;
    return tiles != 0;
}

// One threadgroup: counts the tiles of each stream (a thread takes a run of consecutive streams), scans the counts
// and publishes the dispatch for the decode kernel.
kernel void gdeflate_prepare(constant GDParams &p [[buffer(0)]], uint tid [[thread_index_in_threadgroup]],
                             uint tg_size [[threads_per_threadgroup]], uint sg [[simdgroup_index_in_threadgroup]],
                             uint lane [[thread_index_in_simdgroup]])
{
    threadgroup uint partial[32];
    device uint *scratch = reinterpret_cast<device uint *>(p.scratch);
    device const uint *control = reinterpret_cast<device const uint *>(p.control);
    const uint n = p.control_size >= 4 ? ctl_count(p) : 0u;
    const uint run = (n + tg_size - 1) / tg_size;
    const uint first = min(tid * run, n), last = min(first + run, n);
    uint local = 0;
    for (uint i = first; i < last; ++i) {
        uint tiles;
        if (!stream_valid(p, control[1 + 2 * i], tiles))
            tiles = 0;
        scratch[kScratchPrefix + i] = tiles;
        local += tiles;
    }
    const uint before = simd_prefix_exclusive_sum(local);
    const uint sum = simd_sum(local);   // (all lanes take part)
    if (lane == 0)
        partial[sg] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint base = 0, total = 0;
    for (uint s = 0; s < (tg_size + 31) / 32; ++s) {
        if (s < sg)
            base += partial[s];
        total += partial[s];
    }
    uint running = base + before;
    for (uint i = first; i < last; ++i) {
        const uint tiles = scratch[kScratchPrefix + i];
        scratch[kScratchPrefix + i] = running;
        running += tiles;
    }
    if (tid == 0) {
        scratch[kScratchPrefix + n] = total;
        scratch[0] = total == 0 ? 0u : min(total, kRowWidth);
        scratch[1] = (total + kRowWidth - 1) / kRowWidth;
        scratch[2] = 1;
        scratch[3] = n;
    }
}

// ------------------------------------------------------------------------------------------------------
// Decoder. One SIMD group of 32 lanes per tile; lane i decodes bitstream i.

struct Shared {
    atomic_uint tmp[32];
    atomic_uint lens[64];     // 4-bit code lengths, 8 per dword: literal/length codes, then distance codes
    uint symbols[320];        // literal/length symbols sorted by (code length, value), then distance symbols at 288
};

struct Ctx {
    device const uchar *in;
    uint in_size;
    device uchar *out;
    ulong out_size;
    uint tid;
    bool unaligned;   // the tile's data does not start on a 4-byte boundary
};

struct BitReader {
    uint base, cnt;
    ulong buf;
};

inline uint mask_(uint n) { return (1u << n) - 1u; }
inline uint lt_mask(uint tid) { return (1u << tid) - 1u; }
inline uint vote(bool p) { return uint(ulong(simd_ballot(p))); }
inline uint firstbithigh_(uint x) { return x ? 31u - clz(x) : 0xffffffffu; }
inline uint shuf(uint v, uint lane) { return simd_shuffle(v, ushort(lane & 31u)); }
inline void sync() { threadgroup_barrier(mem_flags::mem_threadgroup); }
inline void sync_device() { threadgroup_barrier(mem_flags::mem_device); }

inline uint ld32(thread const Ctx &c, uint addr)
{
    if (addr > c.in_size || c.in_size - addr < 4) {
        uint v = 0;
        for (uint i = 0; i < 4; ++i) {
            if (ulong(addr) + i < c.in_size)
                v |= uint(c.in[addr + i]) << (8 * i);
        }
        return v;
    }
    if (!c.unaligned)
        return *reinterpret_cast<device const uint *>(c.in + addr);
    return uint(c.in[addr]) | (uint(c.in[addr + 1]) << 8) | (uint(c.in[addr + 2]) << 16) | (uint(c.in[addr + 3]) << 24);
}

inline uint rd_out(thread const Ctx &c, ulong pos) { return pos < c.out_size ? uint(c.out[pos]) : 0u; }
inline void st_out(thread const Ctx &c, ulong pos, uint v)
{
    if (pos < c.out_size)
        c.out[pos] = uchar(v);
}

inline void br_init(thread const Ctx &c, thread BitReader &br, uint pos)
{
    br.cnt = 32;
    br.buf = ld32(c, pos + c.tid * 4);
    br.base = pos + 128;
}

// Refills the bit buffers of the lanes that are low; the 32-bit chunks they read are consecutive in lane order.
inline void br_refill(thread const Ctx &c, thread BitReader &br, bool p)
{
    p = p && br.cnt < 32;
    const uint ballot = vote(p);
    const uint offset = popcount(ballot & lt_mask(c.tid)) * 4;
    if (p) {
        br.buf |= ulong(ld32(c, br.base + offset)) << br.cnt;
        br.cnt += 32;
    }
    br.base += popcount(ballot) * 4;
}

inline void br_eat(thread const Ctx &c, thread BitReader &br, uint n, bool p)
{
    if (p) {
        br.buf >>= n;
        br.cnt -= n;
    }
    br_refill(c, br, p);
}

inline uint br_peek(thread const BitReader &br) { return uint(br.buf); }

inline uint br_read(thread const Ctx &c, thread BitReader &br, uint n, bool p)
{
    const uint bits = p ? uint(br.buf) & mask_(n) : 0u;
    br_eat(c, br, n, p);
    return bits;
}

// Histogram of code lengths of the lanes' code lengths: lane i < `cnt` has length `len`; every lane gets the
// histogram of its 16-lane half (counts[len] at lane `len`).
inline uint get_histogram(threadgroup Shared &sh, uint cnt, uint len, uint tid)
{
    atomic_store_explicit(&sh.tmp[tid], 0u, memory_order_relaxed);
    sync();
    if (len != 0 && tid < cnt)
        atomic_fetch_add_explicit(&sh.tmp[len], 1u, memory_order_relaxed);
    sync();
    const uint v = atomic_load_explicit(&sh.tmp[tid & 15], memory_order_relaxed);
    sync();
    return v;
}

// Rank among the lanes holding the same value (the lanes below this one with an equal value), and their mask.
inline uint match16(uint value, uint tid, thread uint &rank_mask)
{
    uint m = 0;
    for (uint k = 0; k < 16; ++k) {
        const uint b = vote(value == k);
        if (value == k)
            m = b;
    }
    rank_mask = m;
    return m;
}

// Builds a canonical Huffman decoder for two code length alphabets at once, one per half of the SIMD group.
struct Decoder {
    uint base_codes;   // left-aligned first code of the length (lane & 15) + 1, ~0 beyond the longest
    uint offsets;      // number of symbols with a shorter length

    inline uint scan16(uint value, uint tid) const
    {
        for (uint i = 1; i < 16; i *= 2) {
            const uint up = simd_shuffle_up(value, ushort(i));
            if ((tid & 15) >= i)
                value += up;
        }
        return value;
    }

    inline void init(uint counts, uint maxlen, uint tid)
    {
        offsets = scan16(counts, tid);
        uint base_code = 0;
        const uint lane = tid & 15;
        for (uint i = 1; i < maxlen; ++i) {
            const uint count = shuf(counts, (tid & 16) + i);
            if (lane >= i)
                base_code += count << (lane - i);
        }
        const uint tmp = lane == 0 ? base_code : base_code << (32 - lane);
        base_codes = (tmp < base_code || lane >= maxlen) ? 0xffffffffu : tmp;
    }

    inline uint len4code(uint code, uint base) const
    {
        uint len = 1;
        if (code >= shuf(base_codes, 7 + base))
            len = 8;
        if (code >= shuf(base_codes, len + 3 + base))
            len += 4;
        if (code >= shuf(base_codes, len + 1 + base))
            len += 2;
        if (code >= shuf(base_codes, len + base))
            len += 1;
        return len;
    }

    inline uint id4code(uint code, uint len, uint base) const
    {
        const uint i = len + base - 1;
        return shuf(offsets, i) + ((code - shuf(base_codes, i)) >> (32 - len));
    }

    inline uint decode(threadgroup Shared &sh, uint bits, thread uint &len, bool isdist) const
    {
        const uint code = reverse_bits(bits);
        const uint base = isdist ? 16u : 0u;
        len = len4code(code, base);
        const uint index = id4code(code, len, base) + (isdist ? 288u : 0u);
        return sh.symbols[min(index, 319u)];
    }
};

// Scatters symbols into the sorted symbol table: symbol `sym` with code length `len` goes to `offset` plus its rank
// among the lanes with the same length.
inline void scatter(threadgroup Shared &sh, uint sym, uint len, uint offset, uint tid)
{
    uint m;
    match16(len, tid, m);
    if (len != 0)
        sh.symbols[min(offset + popcount(m & lt_mask(tid)), 319u)] = sym;
}

inline uint get4b(threadgroup Shared &sh, uint i)
{
    return (atomic_load_explicit(&sh.lens[(i / 8) & 63], memory_order_relaxed) >> (4 * (i % 8))) & 15u;
}

inline void set4b(threadgroup Shared &sh, uint nibble, uint n, uint i)
{
    uint nibbles = nibble * 0x11111111u;
    nibbles &= mask_(4 * n);   // n <= 6
    const uint base = i / 8, shift = i % 8;
    atomic_fetch_or_explicit(&sh.lens[base & 63], nibbles << (shift * 4), memory_order_relaxed);
    if (shift + n > 8)
        atomic_fetch_or_explicit(&sh.lens[(base + 1) & 63], nibbles >> ((8 - shift) * 4), memory_order_relaxed);
}

// Builds the literal/length and distance symbol tables from the code lengths in sh.lens.
inline void symbol_table_init(threadgroup Shared &sh, uint hlit, uint offsets, uint tid)
{
    if (tid != 15 && tid != 31)
        atomic_store_explicit(&sh.tmp[tid + 1], offsets, memory_order_relaxed);
    sync();
    for (uint i = 0; i < 8; ++i) {
        const uint sym = i * 32 + tid;
        const uint len = get4b(sh, sym);
        const uint start = atomic_load_explicit(&sh.tmp[len], memory_order_relaxed);
        uint m;
        match16(len, tid, m);
        if (len != 0)
            sh.symbols[min(start + popcount(m & lt_mask(tid)), 319u)] = sym;
        sync();
        if (tid == ctz(m))
            atomic_fetch_add_explicit(&sh.tmp[len], popcount(m), memory_order_relaxed);
        sync();
    }
    // literals past 255 (bounded by hlit)
    {
        const uint sym = 8 * 32 + tid;
        const uint len = sym < hlit ? get4b(sh, sym) : 0u;
        const uint start = atomic_load_explicit(&sh.tmp[len], memory_order_relaxed);
        scatter(sh, sym, len, start, tid);
    }
    // distance codes (the source array is padded with zeros)
    {
        const uint len = get4b(sh, tid + hlit);
        const uint start = atomic_load_explicit(&sh.tmp[16 + len], memory_order_relaxed);
        scatter(sh, tid, len, 288 + start, tid);
    }
    sync();
}

constant uint kLane4Id[32] = {3, 17, 15, 13, 11, 9, 7, 5, 4, 6, 8, 10, 12, 14, 16, 18,
                              0, 1,  2,  0,  0,  0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0};

constant uint kCodeLenBase[4] = {1, 3, 3, 11};
constant uint kCodeLenExtra[4] = {0, 2, 3, 7};

// Dynamic blocks: unpacks the code lengths. Returns the histogram of literal/length code lengths in the lower
// lanes and of distance code lengths in the upper lanes.
inline uint unpack_code_lengths(thread const Ctx &c, thread BitReader &br, threadgroup Shared &sh, uint hlit,
                                uint hdist, uint hclen)
{
    const uint tid = c.tid;
    // code length code lengths, 3 bits each in lane order of the reordered alphabet
    uint len = br_read(c, br, 3, tid < hclen);
    len = shuf(len, kLane4Id[tid]);
    len &= tid < 19 ? 0xfu : 0u;

    Decoder dec;
    const uint cnts = get_histogram(sh, 19, len, tid);
    dec.init(cnts, 7, tid);
    const uint offs = shuf(dec.offsets, len ? len - 1 : 0);
    scatter(sh, tid, len, offs, tid);

    const uint count = hlit + hdist;
    uint base_offset = 0;
    uint last_len = ~0u;

    atomic_store_explicit(&sh.lens[tid], 0u, memory_order_relaxed);
    atomic_store_explicit(&sh.lens[tid + 32], 0u, memory_order_relaxed);
    atomic_store_explicit(&sh.tmp[tid], 0u, memory_order_relaxed);
    sync();

    do {
        uint l;
        const uint bits = br_peek(br) & mask_(14);
        const uint sym = dec.decode(sh, bits, l, false);
        const uint idx = sym <= 15 ? 0u : min(sym - 15u, 3u);
        const uint n = kCodeLenBase[idx] + ((bits >> l) & mask_(kCodeLenExtra[idx]));

        // scan back to the nearest lane that holds a length (symbol 16 repeats the previous one)
        const uint lane = firstbithigh_(vote(sym != 16) & lt_mask(tid));
        uint codelen = sym;
        if (sym > 16)
            codelen = 0;
        const uint prevlen = shuf(codelen, lane);
        if (sym == 16)
            codelen = lane == ~0u ? last_len : prevlen;
        last_len = simd_broadcast(codelen, ushort(31));

        base_offset = simd_prefix_exclusive_sum(n) + base_offset;
        if (base_offset < count && codelen != 0) {
            const int i = int(base_offset), ni = int(n), h = int(hlit);
            const int lit = max(min(h - i, ni), 0);
            if (lit != 0)
                atomic_fetch_add_explicit(&sh.tmp[codelen & 15], uint(lit), memory_order_relaxed);
            const int dist = max(min(i + ni - h, ni), 0);
            if (dist != 0)
                atomic_fetch_add_explicit(&sh.tmp[16 + (codelen & 15)], uint(dist), memory_order_relaxed);
            set4b(sh, codelen, n, base_offset);
        }
        br_eat(c, br, l + kCodeLenExtra[idx], base_offset < count);
        base_offset = simd_broadcast(base_offset + n, ushort(31));
    } while (base_offset < count);
    sync();
    const uint hist = atomic_load_explicit(&sh.tmp[tid], memory_order_relaxed);
    sync();
    return hist;
}

constant uint kBaseDist[32] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,   33,
                               49,   65,   97,   129,  193,  257,   385,   513,   769,   1025, 1537,
                               2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153};
constant uint kBaseLength[32] = {0,  3,  4,  5,  6,  7,  8,   9,   10,  11, 13, 15, 17, 19, 23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 3, 0, 0};
constant uint kExtraDist[32] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14};
constant uint kExtraLength[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 16, 0, 0};

// Translates a symbol to its value (length or distance), consuming its code and extra bits.
inline uint translate_symbol(thread const Ctx &c, thread BitReader &br, uint sym, uint len, uint bits, bool isdist,
                             bool p)
{
    const uint s = sym & 31, ls = (sym - 256) & 31;
    const uint base = isdist ? kBaseDist[s] : (sym >= 256 ? kBaseLength[ls] : 1u);
    const uint n = isdist ? kExtraDist[s] : (sym >= 256 ? kExtraLength[ls] : 0u);
    br_eat(c, br, len + n, isdist || p);
    return base + ((bits >> len) & mask_(n));
}

// Literals are stored by their own lanes; each match is copied by all lanes together.
inline void write_output(thread const Ctx &c, ulong dst, uint offset, uint dist, uint length, uint byte, bool iscopy)
{
    const uint tid = c.tid;
    if (!iscopy && length != 0)
        st_out(c, dst + offset, byte);
    uint m = vote(iscopy);
    if (m)
        sync_device();   // the literals are in place before the copies read them
    while (m) {
        const uint lane = ctz(m);
        const uint off = max(shuf(dist, lane), 1u);
        const uint len = shuf(length, lane);
        const ulong output = dst + shuf(offset, lane);
        for (uint i = tid; i < len; i += 32)
            st_out(c, output + i, rd_out(c, output + (i % off) - off));
        sync_device();   // the next copy may read what this one wrote
        m &= m - 1;
    }
}

// Decodes a Huffman-compressed block (the code lengths are in sh.lens). Returns the advanced destination.
inline ulong compressed_block(thread const Ctx &c, thread BitReader &br, threadgroup Shared &sh, uint hlit, uint counts,
                              ulong dst)
{
    const uint tid = c.tid;
    Decoder dec;
    dec.init(counts, 15, tid);
    symbol_table_init(sh, hlit, dec.offsets, tid);

    uint len;
    uint sym = dec.decode(sh, br_peek(br) & mask_(31), len, false);
    uint eob = vote(sym == 256);
    bool oob = (eob & lt_mask(tid)) != 0;
    uint value = translate_symbol(c, br, sym, len, br_peek(br), false, !oob);
    uint length = oob ? 0u : value;
    uint offset = simd_prefix_exclusive_sum(length);
    bool iscopy = sym > 256;
    uint byte = sym;

    while (eob == 0 && br.base <= c.in_size + 4096u) {   // (the bound stops a damaged stream that never ends)
        sym = dec.decode(sh, br_peek(br) & mask_(31), len, iscopy);
        eob = vote(sym == 256);
        oob = (eob & lt_mask(tid)) != 0;
        value = translate_symbol(c, br, sym, len, br_peek(br), iscopy, !oob);
        write_output(c, dst, offset, value, length, byte, iscopy);
        dst += simd_broadcast(offset + length, ushort(31));
        length = (iscopy || oob) ? 0u : value;
        offset = simd_prefix_exclusive_sum(length);
        iscopy = sym > 256;
        byte = sym;
    }
    // the last round: pending distances
    sym = dec.decode(sh, br_peek(br) & mask_(31), len, true);
    iscopy = iscopy && !oob;
    const uint dist = translate_symbol(c, br, sym, len, br_peek(br), iscopy, false);
    write_output(c, dst, offset, dist, length, byte, iscopy);
    const ulong res = dst + simd_broadcast(offset + length, ushort(31));
    sync_device();
    return res;
}

// Stored block: bytes are dealt to the lanes round-robin.
inline ulong stored_block(thread const Ctx &c, thread BitReader &br, ulong dst, uint size)
{
    uint rounds = size / 32;
    while (rounds--) {
        st_out(c, dst + c.tid, br_read(c, br, 8, true));
        dst += 32;
    }
    const uint rem = size % 32;
    if (rem != 0) {
        const uint b = br_read(c, br, 8, c.tid < rem);
        if (c.tid < rem)
            st_out(c, dst + c.tid, b);
        dst += rem;
    }
    return dst;
}

inline uint fixed_code_lengths(threadgroup Shared &sh, uint tid)
{
    atomic_store_explicit(&sh.lens[tid], tid < 18 ? 0x88888888u : 0x99999999u, memory_order_relaxed);
    atomic_store_explicit(&sh.lens[tid + 32], tid < 3 ? 0x77777777u : (tid < 4 ? 0x88888888u : 0x55555555u),
                          memory_order_relaxed);
    sync();
    return tid == 7 ? 24u : (tid == 8 ? 152u : (tid == 9 ? 112u : (tid == 16 + 5 ? 32u : 0u)));
}

// Decompresses one tile: `out_pos` is the tile's offset in the output buffer, `out_size` its size.
inline void decompress_tile(thread Ctx &c, threadgroup Shared &sh, uint in_pos, ulong out_pos)
{
    const uint tid = c.tid;
    BitReader br;
    br_init(c, br, in_pos);
    ulong dst = out_pos;
    bool done;
    do {
        const uint header = simd_broadcast_first(br_peek(br));
        done = (header & 1u) != 0;
        const uint btype = (header >> 1) & 3u;
        br_eat(c, br, 3, tid == 0);

        if (btype == 2) {
            const uint hlit = ((header >> 3) & 31u) + 257u;
            const uint hdist = ((header >> 8) & 31u) + 1u;
            const uint hclen = ((header >> 13) & 15u) + 4u;
            br_eat(c, br, 14, tid == 0);
            sync();
            const uint counts = unpack_code_lengths(c, br, sh, hlit, hdist, hclen);
            dst = compressed_block(c, br, sh, hlit, counts, dst);
        } else if (btype == 1) {
            const uint counts = fixed_code_lengths(sh, tid);
            dst = compressed_block(c, br, sh, 288, counts, dst);
        } else if (btype == 0) {
            const uint size = simd_broadcast_first(br_read(c, br, 16, tid == 0));
            sync_device();
            dst = stored_block(c, br, dst, size);
            sync_device();
        } else {
            break;   // damaged stream
        }
        // a damaged stream may never end: stop at the end of the tile's input
        if (br.base > c.in_size + 4096)
            break;
    } while (!done);
}

// One threadgroup (one SIMD group) per tile of every stream.
kernel void gdeflate_decode(constant GDParams &p [[buffer(0)]], uint2 group [[threadgroup_position_in_grid]],
                            uint tid [[thread_index_in_threadgroup]])
{
    threadgroup Shared sh;
    device const uint *scratch = reinterpret_cast<device const uint *>(p.scratch);
    device const uint *control = reinterpret_cast<device const uint *>(p.control);
    const uint n = scratch[3];
    const uint total = scratch[kScratchPrefix + n];
    const uint tile = group.y * kRowWidth + group.x;
    if (tile >= total)
        return;
    // the stream holding the tile: the last one whose first tile is not past it (streams without tiles share an index)
    uint lo = 0, hi = n;
    while (hi - lo > 1) {
        const uint mid = (lo + hi) / 2;
        if (scratch[kScratchPrefix + mid] <= tile)
            lo = mid;
        else
            hi = mid;
    }
    uint stream = lo;
    while (stream + 1 < n && scratch[kScratchPrefix + stream + 1] <= tile)   // skip empty streams
        ++stream;
    const uint t = tile - scratch[kScratchPrefix + stream];
    const uint in_off = control[1 + 2 * stream];
    const uint out_off = control[2 + 2 * stream];

    const uint w0 = load_dword(p, in_off), w1 = load_dword(p, in_off + 4);
    const uint num_tiles = w0 >> 16;
    const uint table = in_off + kStreamHeaderSize;
    const uint first = t > 0 ? load_dword(p, table + 4 * t) : 0u;
    const uint data_start = table + num_tiles * 4;
    const uint in_pos = data_start + first;
    const uint last_field = (w1 >> 2) & 0x3ffffu;
    const uint last_size = last_field > 0 ? last_field : kTileSize;

    Ctx c;
    c.in = reinterpret_cast<device const uchar *>(p.input);
    c.in_size = uint(min(p.input_size, ulong(0xffffffffu)));
    c.out = reinterpret_cast<device uchar *>(p.output);
    // the tile writes at most its own size; a damaged stream cannot spill into the next tile's bytes
    const ulong out_pos = ulong(out_off) + ulong(t) * kTileSize;
    c.out_size = min(p.output_size, out_pos + (t < num_tiles - 1 ? kTileSize : last_size));
    c.tid = tid;
    c.unaligned = ((p.input + in_pos) & 3) != 0;
    decompress_tile(c, sh, in_pos, out_pos);
}
)msl"
