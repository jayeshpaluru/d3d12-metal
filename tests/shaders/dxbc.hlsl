// Shader Model 5 shaders, compiled to DXBC (not DXIL) by tools/gen-dxbc-test-shaders.sh: the converter must accept
// bytecode of the kind FXC and D3DCompile produce. Same job as color.hlsl and fill.hlsl.
cbuffer Color : register(b0)
{
    float4 color;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 weight : TEXCOORD0;
};

VSOutput VSMain(float3 position : POSITION, uint vertex : SV_VertexID)
{
    VSOutput output;
    output.position = float4(position, 1.0);
    output.weight = float3(vertex == 0, vertex == 1, vertex == 2);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // The weights add up to one, so the colour is exact; this keeps a few interpolants and arithmetic alive.
    return color * dot(input.weight, float3(1, 1, 1));
}

RWStructuredBuffer<uint> output_buffer : register(u0);

[numthreads(8, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint sum = 0;
    for (uint i = 0; i <= id.x; ++i)
        sum += i * 3 + 1;
    output_buffer[id.x] = sum + asuint(color.x);
}
