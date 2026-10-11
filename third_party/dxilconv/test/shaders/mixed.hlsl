StructuredBuffer<uint> a : register(t0);
Buffer<uint> tb : register(t1);
RWByteAddressBuffer o : register(u0);
RWBuffer<uint> ot : register(u1);
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID) { o.Store(t.x*4, a[t.x] + tb[t.x * 5]); ot[t.x] = tb[t.x]; }
