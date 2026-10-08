// Draws geometry in a constant colour. The colour comes from register b0; the
// tests bind that register as root constants, a descriptor table or a root CBV.
cbuffer Color : register(b0)
{
    float4 color;
};

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return color;
}
