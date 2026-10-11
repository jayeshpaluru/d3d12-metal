// Typed buffer views above 2^28 elements (tests/portable/p_typed_windows.cpp). `mode` picks the operation, the element
// indices and values come from a constant buffer, the results go to a raw root UAV.
cbuffer Params : register(b0)
{
    uint mode;
    uint count;
    uint pad0;
    uint pad1;
};

cbuffer Lists : register(b1)
{
    uint4 indices[16];
    uint4 values[16];
};

Buffer<uint> srv_uint : register(t0);
Buffer<float4> srv_float : register(t1);
RWByteAddressBuffer results : register(u0);
RWBuffer<uint> uav_uint : register(u1);

uint index_at(uint i) { return indices[i >> 2][i & 3]; }
uint value_at(uint i) { return values[i >> 2][i & 3]; }

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint t = id.x;
    switch (mode) {
    case 0: // typed UAV stores
        if (t < count)
            uav_uint[index_at(t)] = value_at(t);
        break;
    case 1: // SRV loads
        if (t < count)
            results.Store(t * 4, srv_uint[index_at(t)]);
        break;
    case 2: // typed UAV loads
        if (t < count)
            results.Store(t * 4, uav_uint[index_at(t)]);
        break;
    case 3: { // atomics: add 1 returning the original, add 3, add (t & 7)
        uint original;
        InterlockedAdd(uav_uint[index_at(0)], 1, original);
        results.Store(t * 4, original);
        InterlockedAdd(uav_uint[index_at(1)], 3);
        InterlockedAdd(uav_uint[index_at(2)], t & 7);
        break;
    }
    case 4: // float4 SRV loads
        if (t < count)
            results.Store4(t * 16, asuint(srv_float[index_at(t)]));
        break;
    }
}
