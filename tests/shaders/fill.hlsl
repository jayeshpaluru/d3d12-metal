// Compute shaders for the UAV tests. Entry point per test mode, selected by CSMain's constant `mode`.
cbuffer Params : register(b0)
{
    uint2 size;
    uint mode;
    uint value;
};

RWTexture2D<float4> out_texture : register(u0);
RWStructuredBuffer<uint> out_buffer : register(u1);
RWByteAddressBuffer raw_buffer : register(u2);
RWStructuredBuffer<uint> appended : register(u3);   // with a counter
RWTexture2D<float> float_texture : register(u4);    // typed UAV load and store
RWBuffer<uint> typed_buffer : register(u5);         // typed UAV load and store

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    if (id.x >= size.x || id.y >= size.y)
        return;
    const uint linear_index = id.y * size.x + id.x;
    switch (mode) {
    case 0:  // a pattern into a texture and a buffer
        out_texture[id.xy] = float4(id.x / 255.0, id.y / 255.0, (id.x ^ id.y) / 255.0, 1.0);
        out_buffer[linear_index] = 0xA5000000u | (id.y << 8) | id.x;
        break;
    case 1:  // read what mode 0 wrote (after a UAV barrier) and transform it
        out_buffer[linear_index] = out_buffer[linear_index] + value;
        break;
    case 2:  // the counter: every thread takes a slot
        {
            uint slot = appended.IncrementCounter();
            appended[slot] = linear_index + 1;
        }
        break;
    case 3:  // typed loads and stores
        float_texture[id.xy] = float_texture[id.xy] * 2.0 + 1.0;
        typed_buffer[linear_index] = typed_buffer[linear_index] + value;
        break;
    case 4:  // byte address buffer
        raw_buffer.Store(linear_index * 4, linear_index * 3 + value);
        break;
    case 6:  // vertex data for a triangle: x, y, z of three vertices
        if (linear_index < 9) {
            const float v[9] = {-0.8, -0.8, 0.5, 0.8, -0.8, 0.5, 0.0, 0.8, 0.5};
            out_buffer[linear_index] = asuint(v[linear_index] * (value ? float(value) / 100.0 : 1.0));
        }
        break;
    case 5:  // group ids and thread index
        out_buffer[linear_index] = (group.x << 16) | (group.y << 8) | index;
        break;
    }
}
