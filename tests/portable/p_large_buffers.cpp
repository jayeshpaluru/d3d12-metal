// Typed buffer views above the texture buffer limit. Metal texture buffers hold at most 2^28 texels (measured
// on Apple GPUs); the game this layer targets makes 419,430,400-element R16_UINT views of an 800 MB buffer.
// Such a view is clamped to the limit: elements up to the limit read as stored, the rest read as zero.
#include "probe.h"

namespace {

constexpr UINT64 kLimit = 1ull << 28;
constexpr UINT64 kElements = 419430400;  // the game's view
constexpr UINT64 kBufferSize = kElements * 2;

void write_u16(Gpu &gpu, ID3D12Resource *buffer, UINT64 element, uint16_t value)
{
    // Elements are written two at a time so every copy is 4-byte aligned.
    const uint32_t word = value;
    ComPtr<ID3D12Resource> upload = gpu.upload_buffer(&word, 4);
    gpu.run([&](ID3D12GraphicsCommandList *list) { list->CopyBufferRegion(buffer, element * 2, upload.Get(), 0, 4); });
}

} // namespace

int main()
{
    Gpu gpu;
    ComPtr<ID3D12Resource> buffer = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // Markers at the start, just below the limit and past it (element indices, multiples of 16 so that they are exact as the float coordinates the probe takes).
    write_u16(gpu, buffer.Get(), 10, 1234);
    write_u16(gpu, buffer.Get(), kLimit - 32, 4321);
    write_u16(gpu, buffer.Get(), kLimit + 64, 999);
    write_u16(gpu, buffer.Get(), kElements - 32, 777);

    Probe probe(gpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R16_UINT);
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = static_cast<UINT>(kElements);
    probe.set_srv(4, buffer.Get(), srv);

    const std::vector<Float4> got = probe.run(5, {{10, 0, 0, 0},
                                                   {11, 0, 0, 0},
                                                   {float(kLimit - 32), 0, 0, 0},
                                                   {float(kLimit + 64), 0, 0, 0},
                                                   {float(kElements - 32), 0, 0, 0}});
    CHECK_EQ(int(got[0].x), 1234);
    CHECK_EQ(int(got[1].x), 0);
    CHECK_EQ(int(got[2].x), 4321);  // the last elements below the limit are intact
    // Past the limit the clamped view reads zero instead of 999 and 777.
    CHECK_EQ(int(got[3].x), 0);
    CHECK_EQ(int(got[4].x), 0);

    // The same view as a UAV (typed loads) and a write near the start.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_R16_UINT;
    uav.Buffer.NumElements = static_cast<UINT>(kElements);
    gpu.device->CreateUnorderedAccessView(buffer.Get(), nullptr, &uav, gpu.cpu_handle(probe.srv_heap.Get(), 7));
    const std::vector<Float4> loaded = probe.run(9, {{10, 0, 0, 0}, {float(kLimit - 32), 0, 0, 0}});
    CHECK_EQ(int(loaded[0].x), 1234);
    CHECK_EQ(int(loaded[1].x), 4321);
    probe.run(10, {{20, 55, 0, 0}});
    const std::vector<uint8_t> bytes = gpu.read_buffer(buffer.Get(), 64);
    uint16_t halves[32];
    std::memcpy(halves, bytes.data(), 64);
    CHECK_EQ(halves[20], 55);
    CHECK_EQ(halves[10], 1234);

    // A view that starts inside the buffer and ends past the limit keeps its first elements.
    srv.Buffer.FirstElement = 16;
    srv.Buffer.NumElements = static_cast<UINT>(kElements - 16);
    probe.set_srv(4, buffer.Get(), srv);
    const std::vector<Float4> offset = probe.run(5, {{float(kLimit - 32 - 16), 0, 0, 0}});
    CHECK_EQ(int(offset[0].x), 4321);

    std::printf("p_large_buffers: PASS\n");
    return 0;
}
