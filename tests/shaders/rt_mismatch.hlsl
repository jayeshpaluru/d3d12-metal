// SPDX-License-Identifier: LGPL-2.1-or-later
// The pixel shader writes an integer to target 1 and a float colour to target 0; a pipeline may bind both targets
// with normalised formats (D3D12 allows it; the integer output has no defined effect).
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

struct PSOutput
{
    float4 colour : SV_Target0;
    uint4 id : SV_Target1;
};

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    output.colour = color;
    output.id = uint4(1, 2, 3, 4);
    return output;
}
