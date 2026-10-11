Buffer<float4> src : register(t0);
RWBuffer<uint> dst : register(u0);
RWStructuredBuffer<float4> outb : register(u1);
cbuffer C : register(b0) { uint count; uint base; };
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID) {
  float4 sum = 0;
  for (uint i = 0; i < count; ++i) {
    float4 v = src[base + i * 100000000u + t.x];
    if (v.x > 1.0) { dst[base + i + t.x] = asuint(v.y); sum += v; } else { sum -= v.wzyx; }
    uint ignored;
    InterlockedAdd(dst[i], 1, ignored);
  }
  outb[t.x] = sum;
}
