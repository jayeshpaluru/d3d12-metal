StructuredBuffer<uint> a : register(t0);
ByteAddressBuffer b : register(t1);
RWByteAddressBuffer o : register(u0);
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID) { o.Store(t.x*4, a[t.x] + b.Load(t.x*4)); }
