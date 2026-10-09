// SPDX-License-Identifier: LGPL-2.1-or-later
// Draws a small square at a position from the vertex buffer, moved by a per-instance offset and coloured by a
// per-instance colour. Vertex slot 0 is per-vertex, slot 1 per-instance.
struct VSInput
{
    float2 position : POSITION;
    float2 offset : OFFSET;
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
    output.position = float4(input.position + input.offset, 0.5, 1.0);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.color;
}
