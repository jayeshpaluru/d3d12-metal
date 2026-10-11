// SPDX-License-Identifier: LGPL-2.1-or-later
// Rasterizer ordered views: instance i of a full-screen triangle does value = value * 31 + i + 1 on each pixel's element of a
// buffer. Applied in submission order (what a ROV guarantees) the result is one fixed number per pixel; a plain UAV
// gives that only when the hardware happens to keep overlapping fragments in order. Register b0 holds the width, u0 is
// the ordered buffer, u1 the plain one.
cbuffer Size : register(b0)
{
    uint width;
    uint3 padding;
};

RasterizerOrderedStructuredBuffer<uint> ordered : register(u0);
RWStructuredBuffer<uint> plain : register(u1);
RasterizerOrderedTexture2D<uint> ordered_texture : register(u2);
RWTexture2D<uint> plain_texture : register(u3);

struct VSOutput
{
    float4 position : SV_Position;
    nointerpolation uint instance : INSTANCE;
};

VSOutput VSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    VSOutput output;
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    output.instance = instance;
    return output;
}

float4 PSOrdered(VSOutput input) : SV_Target
{
    const uint index = uint(input.position.y) * width + uint(input.position.x);
    ordered[index] = ordered[index] * 31u + input.instance + 1u;
    return float4(0, 0, 0, 0);
}

float4 PSPlain(VSOutput input) : SV_Target
{
    const uint index = uint(input.position.y) * width + uint(input.position.x);
    plain[index] = plain[index] * 31u + input.instance + 1u;
    return float4(0, 0, 0, 0);
}

float4 PSOrderedTexture(VSOutput input) : SV_Target
{
    const uint2 pixel = uint2(input.position.xy);
    ordered_texture[pixel] = ordered_texture[pixel] * 31u + input.instance + 1u;
    return float4(0, 0, 0, 0);
}

float4 PSPlainTexture(VSOutput input) : SV_Target
{
    const uint2 pixel = uint2(input.position.xy);
    plain_texture[pixel] = plain_texture[pixel] * 31u + input.instance + 1u;
    return float4(0, 0, 0, 0);
}
