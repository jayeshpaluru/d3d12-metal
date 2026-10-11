Buffer<uint> src : register(t3);
RWBuffer<uint> dst : register(u1);
RWBuffer<int> acc : register(u2);
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID) {
  uint v = src[t.x * 1000000];
  dst[t.x] = v;
  int o; InterlockedAdd(acc[t.x], 1, o);
  InterlockedAdd(acc[t.x + 5], 1);
  int o2; InterlockedCompareExchange(acc[t.x], 1, 2, o2);
  dst[t.x + 1] = o + o2;
}
