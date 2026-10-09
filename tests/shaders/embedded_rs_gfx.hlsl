// SPDX-License-Identifier: LGPL-2.1-or-later
// A pixel shader that carries its root signature (the vertex shader does not): a graphics pipeline made without a root
// signature takes it from whichever stage has one.
cbuffer Color : register(b0)
{
    float4 color;
};

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(float3 position : POSITION)
{
    VSOutput output;
    output.position = float4(position, 1.0);
    return output;
}

[RootSignature("RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), RootConstants(num32BitConstants=4, b0)")]
float4 PSMain(VSOutput input) : SV_Target
{
    return color;
}
