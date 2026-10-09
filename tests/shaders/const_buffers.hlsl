// SPDX-License-Identifier: LGPL-2.1-or-later
// D3D12HelloConstBuffers: a triangle moved by a constant buffer (shaders.hlsl of the sample).
cbuffer SceneConstantBuffer : register(b0)
{
    float4 offset;
    float4 padding[15];
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(float4 position : POSITION, float4 color : COLOR)
{
    PSInput result;
    result.position = position + offset;
    result.color = color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
