// Passes the vertex buffer's position through unchanged, so a test can give a vertex an infinite position (games cull
// vertices that way: Spider-Man 2 writes (+inf, 0, 0, 0) for hair strand vertices it drops).
struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VSMain(float4 position : POSITION)
{
    VSOutput output;
    output.position = position;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(1, 0, 0, 1);
}
