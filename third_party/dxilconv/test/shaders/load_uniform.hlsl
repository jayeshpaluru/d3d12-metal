Buffer<uint> src : register(t3);
RWStructuredBuffer<uint> outb : register(u0);
cbuffer C : register(b0) { uint idx; };
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID) { outb[t.x] = src[idx] + src[idx + 1]; }
