// GPU test: thread i reads src[idxs[i]] into outv[i], then writes dst[idxs[i]] = idxs[i] ^ 0xABCD and
// atomically adds 1 to acc[idxs[i]] (returning the old value in olds[i]).
Buffer<uint> src : register(t0);
StructuredBuffer<uint> idxs : register(t1);
RWBuffer<uint> dst : register(u0);
RWBuffer<uint> acc : register(u1);
RWStructuredBuffer<uint> outv : register(u2);
RWStructuredBuffer<uint> olds : register(u3);
[numthreads(1,1,1)] void main(uint3 t : SV_DispatchThreadID) {
  uint i = t.x;
  uint idx = idxs[i];
  outv[i] = src[idx];
  dst[idx] = idx ^ 0xABCDu;
  uint o;
  InterlockedAdd(acc[idx], 1, o);
  olds[i] = o;
}
