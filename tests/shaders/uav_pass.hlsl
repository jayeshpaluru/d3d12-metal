// SPDX-License-Identifier: LGPL-2.1-or-later
// Draws that write a UAV from the pixel shader and draws that read the same buffer from the vertex shader. Register b0
// is a colour, t0 the buffer as an SRV, u0 the buffer as a UAV.
cbuffer Color : register(b0)
{
    float4 color;
};

StructuredBuffer<uint> source : register(t0);
RWStructuredBuffer<uint> sink : register(u0);

struct VSOutput
{
    float4 position : SV_Position;
    nointerpolation float4 tint : TINT;
};

VSOutput VSMain(float3 position : POSITION)
{
    VSOutput output;
    output.position = float4(position, 1.0);
    output.tint = color;
    return output;
}

// Reads what the pixel shader of an earlier draw wrote: green when it is there, red when it is not.
VSOutput VSRead(float3 position : POSITION)
{
    VSOutput output;
    output.position = float4(position, 1.0);
    output.tint = source[0] == 42 ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.tint;
}

float4 PSWrite(VSOutput input) : SV_Target
{
    sink[0] = 42;
    return input.tint;
}
