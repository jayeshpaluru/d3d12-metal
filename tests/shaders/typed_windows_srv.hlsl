// typed_windows.hlsl's SRV loads (mode 1) in a shader whose only typed buffers are SRVs (tests/portable/p_typed_windows.cpp):
// such shaders must be rewritten too.
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
RWByteAddressBuffer results : register(u0);

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < count)
        results.Store(id.x * 4, srv_uint[indices[id.x >> 2][id.x & 3]]);
}
