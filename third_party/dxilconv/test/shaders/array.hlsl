Buffer<uint> arr[4] : register(t2);
RWStructuredBuffer<uint> outb : register(u0);
cbuffer C : register(b0) { uint which; };
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID) { outb[t.x] = arr[which][t.x << 8] + arr[t.x & 3][3]; }
