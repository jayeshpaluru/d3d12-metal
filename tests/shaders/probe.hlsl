// Reads resources at coordinates given by the test and writes what it saw to a UAV buffer: the tests
// check descriptors (texture and buffer views, samplers, swizzles) by looking at what a shader gets.
Texture2D<float4> t2d : register(t0);
Texture2DArray<float4> t2da : register(t1);
TextureCube<float4> tcube : register(t2);
Texture3D<float4> t3d : register(t3);
Buffer<uint> tbuf : register(t4);
ByteAddressBuffer rawbuf : register(t5);
StructuredBuffer<float4> sbuf : register(t6);
SamplerState samp : register(s0);

cbuffer Params : register(b0)
{
    uint mode;
    uint count;
    uint pad0;
    uint pad1;
    float4 coords[64];
};

RWStructuredBuffer<float4> results : register(u0);
RWBuffer<uint> utyped : register(u1);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= count)
        return;
    float4 c = coords[id.x];
    float4 r = 0;
    switch (mode) {
    case 0: r = t2d.SampleLevel(samp, c.xy, c.w); break;
    case 1: r = t2da.SampleLevel(samp, c.xyz, c.w); break;
    case 2: r = tcube.SampleLevel(samp, c.xyz, c.w); break;
    case 3: r = t3d.SampleLevel(samp, c.xyz, c.w); break;
    case 4: r = t2d.Load(int3(c.xy, c.w)); break;
    case 5: r = float4(tbuf[(uint)c.x], 0, 0, 0); break;
    case 6: r = asfloat(rawbuf.Load4((uint)c.x * 4)); break;
    case 7: r = sbuf[(uint)c.x]; break;
    case 9: r = float4(utyped[(uint)c.x], 0, 0, 0); break;
    case 10: utyped[(uint)c.x] = (uint)c.y; break;
    case 8: r = t2d.SampleGrad(samp, c.xy, float2(c.w, 0), float2(0, c.w)); break;
    }
    results[id.x] = r;
}
