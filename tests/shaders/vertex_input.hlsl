// SPDX-License-Identifier: LGPL-2.1-or-later
// Interpolates a per-vertex colour. Position and offset come from one vertex
// buffer slot, the colour (packed 8-bit) from another.
struct VSInput
{
    float3 position : POSITION;
    float2 offset : TEXCOORD;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position.xy + input.offset, input.position.z, 1.0);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.color;
}
