// SPDX-License-Identifier: LGPL-2.1-or-later
// Hull and domain shader emulation: one triangle patch, tessellated 4x; the domain shader passes the
// barycentric coordinates on as the colour, which proves it ran per generated vertex.
struct VSOutput
{
    float4 position : POSITION;
};

struct DSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR;
};

struct PatchConstants
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

VSOutput VSMain(float2 position : POSITION)
{
    VSOutput output;
    output.position = float4(position, 0.0, 1.0);
    return output;
}

PatchConstants PatchConstantFunction(InputPatch<VSOutput, 3> patch)
{
    PatchConstants c;
    c.edges[0] = c.edges[1] = c.edges[2] = 4.0;
    c.inside = 4.0;
    return c;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantFunction")]
VSOutput HSMain(InputPatch<VSOutput, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
DSOutput DSMain(PatchConstants constants, float3 bary : SV_DomainLocation, const OutputPatch<VSOutput, 3> patch)
{
    DSOutput output;
    output.position = bary.x * patch[0].position + bary.y * patch[1].position + bary.z * patch[2].position;
    output.color = bary;
    return output;
}

float4 PSMain(DSOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
