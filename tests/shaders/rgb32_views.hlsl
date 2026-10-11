// Typed buffer SRVs of the 96-bit formats (tests/portable/p_rgb32_views.cpp). `mode` picks the view type; the loaded
// elements (x, y, z, 1) go to a raw root UAV as 16 bytes each.
cbuffer Params : register(b0)
{
    uint mode;
    uint count;
    uint pad0;
    uint pad1;
};

Buffer<uint4> srv_uint : register(t0);
Buffer<int4> srv_sint : register(t1);
Buffer<float4> srv_float : register(t2);
RWByteAddressBuffer results : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= count)
        return;
    switch (mode) {
    case 0:
        results.Store4(id.x * 16, srv_uint[id.x]);
        break;
    case 1:
        results.Store4(id.x * 16, asuint(srv_sint[id.x]));
        break;
    case 2:
        results.Store4(id.x * 16, asuint(srv_float[id.x]));
        break;
    }
}
