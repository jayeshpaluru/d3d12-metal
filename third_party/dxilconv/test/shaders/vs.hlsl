Buffer<float4> pos : register(t0);
float4 main(uint vid : SV_VertexID) : SV_Position { return pos[vid * 3000000]; }
