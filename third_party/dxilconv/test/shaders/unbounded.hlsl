Buffer<uint> arr[] : register(t4, space2);
RWBuffer<uint> uarr[8] : register(u2, space2);
RWStructuredBuffer<uint> outb : register(u0);
cbuffer C : register(b0) { uint which; };
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID) {
  uint v = arr[NonUniformResourceIndex(which + t.x)][t.x << 20];
  uarr[which][t.x << 20] = v;
  outb[t.x] = v;
}
