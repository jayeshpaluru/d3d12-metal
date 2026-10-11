Buffer<float4> src : register(t0, space1);
Buffer<int> isrc : register(t5);
RWStructuredBuffer<float4> outb : register(u0);
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID) {
  float4 v = src[t.x * 70000];
  outb[t.x] = float4(v.x, v.w + isrc[t.x], 0, 0);
}
