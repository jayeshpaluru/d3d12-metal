// Geometry shader emulation: each point becomes a square of half-size 0.1 (a triangle strip of four vertices).
struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(float2 position : POSITION)
{
    VSOutput output;
    output.position = float4(position, 0.0, 1.0);
    return output;
}

[maxvertexcount(4)]
void GSMain(point VSOutput input[1], inout TriangleStream<VSOutput> stream)
{
    const float h = 0.1;
    const float2 c = input[0].position.xy;
    const float2 corners[4] = {float2(-h, -h), float2(-h, h), float2(h, -h), float2(h, h)};
    for (int i = 0; i < 4; ++i) {
        VSOutput v;
        v.position = float4(c + corners[i], 0.0, 1.0);
        stream.Append(v);
    }
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
