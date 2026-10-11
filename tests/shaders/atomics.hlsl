// Atomics on every kind of UAV, heavily contended: 64 threads x 1024 groups hit 61 slots. `mode` picks the case
// (see tests/portable/p_atomics.cpp for the expected values).
cbuffer Params : register(b0)
{
    uint mode;
};

RWBuffer<int> sint_a : register(u0);
RWBuffer<int> sint_b : register(u1);        // a view with a non-zero FirstElement
RWBuffer<uint> uint_buf : register(u2);
RWTexture2D<uint> uint_tex : register(u3);
RWTexture2D<int> sint_tex : register(u4);
RWByteAddressBuffer raw_buf : register(u5);
RWStructuredBuffer<uint> structured : register(u6);
RWStructuredBuffer<uint> originals : register(u7);

static const uint kThreads = 65536;
static const uint kSlots = 61;

void sint_case(RWBuffer<int> buf, uint tid)
{
    const uint slot = tid % kSlots;
    const int value = (tid & 1) ? int(tid % 7) + 1 : -int(tid % 5);
    const int extreme = int((tid * 7) % 1000) - 500;
    InterlockedAdd(buf[slot], value);
    InterlockedMin(buf[64 + slot], extreme);
    InterlockedMax(buf[128 + slot], extreme);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint tid = id.x;
    const uint slot = tid % kSlots;
    switch (mode) {
    case 0:  // initialize everything
        if (tid < 256) {
            const int min_init = (tid >= 64 && tid < 128) ? 2147483647 : 0;
            const int init = (tid >= 128 && tid < 192) ? -2147483647 - 1 : min_init;
            sint_a[tid] = init;
            sint_b[tid] = init;
            uint_buf[tid] = 0;
            uint_tex[uint2(tid % 64, tid / 64)] = 0;
            sint_tex[uint2(tid % 64, tid / 64)] = 0;
            raw_buf.Store(tid * 4, 0);
            structured[tid] = 0;
        }
        break;
    case 1:
        sint_case(sint_a, tid);
        break;
    case 2:
        sint_case(sint_b, tid);
        break;
    case 3:
        InterlockedAdd(uint_buf[slot], tid + 1);
        InterlockedOr(uint_buf[64 + slot], 1u << (tid % 32));
        {
            // a compare-exchange loop incrementing a counter
            uint seen = uint_buf[128 + slot];
            for (;;) {
                uint previous;
                InterlockedCompareExchange(uint_buf[128 + slot], seen, seen + 1, previous);
                if (previous == seen)
                    break;
                seen = previous;
            }
        }
        break;
    case 4:
        InterlockedAdd(uint_tex[uint2(slot, 0)], tid + 1);
        InterlockedMax(uint_tex[uint2(slot, 1)], (tid * 13) % 9973);
        break;
    case 5:
        InterlockedAdd(sint_tex[uint2(slot, 0)], (tid & 1) ? int(tid % 7) + 1 : -int(tid % 5));
        break;
    case 6:
        raw_buf.InterlockedAdd(slot * 4, tid + 1);
        InterlockedAdd(structured[slot], tid + 1);
        break;
    case 8:  // plain typed store and load through the offset view
        if (tid < 256) {
            sint_b[tid] = int(tid) * 3;
            sint_a[tid] = sint_b[tid] + 1;
        }
        break;
    case 7:  // return values: every thread adds 1 to one slot and gets a distinct original
        {
            uint original;
            InterlockedAdd(sint_a[200], 1, original);
            originals[tid] = original;
            InterlockedAdd(sint_b[200], 1, original);
            originals[kThreads + tid] = original;
            InterlockedAdd(uint_buf[200], 1, original);
            originals[2 * kThreads + tid] = original;
            InterlockedAdd(uint_tex[uint2(62, 2)], 1, original);
            originals[3 * kThreads + tid] = original;
            InterlockedAdd(sint_tex[uint2(62, 2)], 1, original);
            originals[4 * kThreads + tid] = original;
            raw_buf.InterlockedAdd(200 * 4, 1, original);
            originals[5 * kThreads + tid] = original;
            InterlockedAdd(structured[200], 1, original);
            originals[6 * kThreads + tid] = original;
        }
        break;
    }
}
