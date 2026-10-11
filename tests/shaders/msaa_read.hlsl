// SPDX-License-Identifier: LGPL-2.1-or-later
// Reads every sample of one pixel of a multisampled texture.
Texture2DMS<float4> ms : register(t0);
RWStructuredBuffer<float4> results : register(u0);

cbuffer Params : register(b0)
{
    uint2 pixel;
    uint samples;
    uint pad;
};

[numthreads(8, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= samples)
        return;
    results[id.x] = ms.Load(int2(pixel), id.x);
}
