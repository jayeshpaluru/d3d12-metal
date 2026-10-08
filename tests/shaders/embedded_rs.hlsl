// A compute shader that carries its root signature (the RTS0 part of the DXIL container): pipelines made from it
// need no root signature of their own. Writes the thread index plus a root constant into a root UAV.
RWStructuredBuffer<uint> out_buffer : register(u0);

cbuffer Params : register(b0)
{
    uint base;
};

[RootSignature("RootConstants(num32BitConstants=1, b0), UAV(u0)")]
[numthreads(16, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    out_buffer[id.x] = base + id.x;
}
