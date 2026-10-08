// Heaps and placed resources: creation, placement and aliasing of buffers and textures, allocation info,
// the forced initial clear of placed render targets and depth-stencils, residency calls, and the calls that
// are deliberately unsupported (reserved resources).
#include "probe.h"

namespace {

constexpr UINT64 kMiB = 1024 * 1024;

UINT64 align_up(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

D3D12_HEAP_DESC heap_desc(D3D12_HEAP_TYPE type, UINT64 size)
{
    D3D12_HEAP_DESC desc = {};
    desc.SizeInBytes = size;
    desc.Properties = heap_props(type);
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Flags = D3D12_HEAP_FLAG_NONE;
    return desc;
}

ComPtr<ID3D12Heap> create_heap(Gpu &gpu, D3D12_HEAP_TYPE type, UINT64 size)
{
    const D3D12_HEAP_DESC desc = heap_desc(type, size);
    ComPtr<ID3D12Heap> heap;
    CHECK_HR(gpu.device->CreateHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf())));
    return heap;
}

ComPtr<ID3D12Resource> place(Gpu &gpu, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC &desc,
                             D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON)
{
    ComPtr<ID3D12Resource> resource;
    CHECK_HR(gpu.device->CreatePlacedResource(heap, offset, &desc, state, nullptr, IID_PPV_ARGS(resource.GetAddressOf())));
    return resource;
}

} // namespace

int main()
{
    Gpu gpu;

    // ---- Heaps ---------------------------------------------------------------------------------------------------
    ComPtr<ID3D12Heap> heap = create_heap(gpu, D3D12_HEAP_TYPE_DEFAULT, 8 * kMiB);
    CHECK_EQ(heap->GetDesc().SizeInBytes, 8 * kMiB);
    CHECK(heap->GetDesc().Properties.Type == D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Heap1> heap1;
    CHECK_HR(heap.As(&heap1));
    D3D12_HEAP_DESC zero = heap_desc(D3D12_HEAP_TYPE_DEFAULT, 0);
    ComPtr<ID3D12Heap> refused;
    CHECK(gpu.device->CreateHeap(&zero, IID_PPV_ARGS(refused.GetAddressOf())) == E_INVALIDARG);

    // ---- Placed buffers, aliasing ---------------------------------------------------------------------------------
    {
        const D3D12_RESOURCE_DESC desc = buffer_desc(64 * 1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> a = place(gpu, heap.Get(), 0, desc);
        ComPtr<ID3D12Resource> b = place(gpu, heap.Get(), 64 * 1024, desc);
        ComPtr<ID3D12Resource> alias = place(gpu, heap.Get(), 0, buffer_desc(32 * 1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
        CHECK(a->GetGPUVirtualAddress() != 0 && b->GetGPUVirtualAddress() != a->GetGPUVirtualAddress());
        CHECK_EQ(alias->GetGPUVirtualAddress(), a->GetGPUVirtualAddress());
        D3D12_HEAP_PROPERTIES props;
        D3D12_HEAP_FLAGS flags;
        CHECK_HR(a->GetHeapProperties(&props, &flags));
        CHECK(props.Type == D3D12_HEAP_TYPE_DEFAULT);

        std::vector<uint32_t> data(1024);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = 0x1000 + static_cast<uint32_t>(i);
        ComPtr<ID3D12Resource> upload = gpu.upload_buffer(data.data(), data.size() * 4);
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->CopyBufferRegion(a.Get(), 0, upload.Get(), 0, data.size() * 4);
            list->CopyBufferRegion(b.Get(), 0, upload.Get(), 0, data.size() * 2);
        });
        // The aliasing buffer sees what was written through the other one.
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            const D3D12_RESOURCE_BARRIER barrier = aliasing_barrier(a.Get(), alias.Get());
            list->ResourceBarrier(1, &barrier);
        });
        std::vector<uint8_t> bytes = gpu.read_buffer(alias.Get(), 4096);
        CHECK(std::memcmp(bytes.data(), data.data(), 4096) == 0);
        bytes = gpu.read_buffer(b.Get(), 2048);
        CHECK(std::memcmp(bytes.data(), data.data(), 2048) == 0);

        // Destroying one of two buffers sharing a start address keeps the other usable by address.
        a.Reset();
        bytes = gpu.read_buffer(alias.Get(), 256);
        CHECK(std::memcmp(bytes.data(), data.data(), 256) == 0);
        Probe probe(gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_UNKNOWN);
        srv.Buffer.NumElements = 4;
        srv.Buffer.StructureByteStride = 16;
        probe.set_srv(6, alias.Get(), srv);
        uint32_t bits[4];
        const Float4 first = probe.one(7, {1, 0, 0, 0});
        std::memcpy(bits, &first, 16);
        CHECK_EQ(bits[0], 0x1004u);
    }

    // ---- Placed textures ---------------------------------------------------------------------------------------------
    {
        // A texture placed in a heap is an ordinary texture to everything else.
        ComPtr<ID3D12Resource> texture = place(gpu, heap.Get(), 2 * kMiB, tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, D3D12_RESOURCE_FLAG_NONE, 1, 3));
        std::vector<uint32_t> pixels(64 * 64);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = 0xff000000u | static_cast<uint32_t>(i & 0xffffff);
        gpu.upload_texture(texture.Get(), 0, pixels.data(), 4);
        Image image = gpu.read_texture(texture.Get(), 0, 4);
        CHECK(std::memcmp(image.data.data(), pixels.data(), pixels.size() * 4) == 0);
        Probe probe(gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
        srv.Texture2D.MipLevels = 3;
        probe.set_srv(0, texture.Get(), srv);
        const Float4 texel = probe.one(4, {5, 2, 0, 0});
        const uint32_t expected = pixels[2 * 64 + 5];
        CHECK(std::fabs(texel.x - (expected & 0xff) / 255.0f) < 0.01f);
        CHECK(std::fabs(texel.y - ((expected >> 8) & 0xff) / 255.0f) < 0.01f);

        // Misaligned and out-of-range placements are refused.
        ComPtr<ID3D12Resource> bad;
        const D3D12_RESOURCE_DESC desc = tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64);
        CHECK(FAILED(gpu.device->CreatePlacedResource(heap.Get(), 8 * kMiB - 4096, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(bad.GetAddressOf()))));
        CHECK(FAILED(gpu.device->CreatePlacedResource(heap.Get(), 100, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(bad.GetAddressOf()))));
    }

    // ---- Forced initial state: a render target placed over old data starts cleared ------------------------------------
    {
        ComPtr<ID3D12Heap> scratch = create_heap(gpu, D3D12_HEAP_TYPE_DEFAULT, 4 * kMiB);
        // Fill the memory with garbage through a buffer.
        const UINT64 garbage_size = 2 * kMiB;
        ComPtr<ID3D12Resource> garbage = place(gpu, scratch.Get(), 0, buffer_desc(garbage_size));
        std::vector<uint8_t> fill(garbage_size, 0xA7);
        ComPtr<ID3D12Resource> upload = gpu.upload_buffer(fill.data(), fill.size());
        gpu.run([&](ID3D12GraphicsCommandList *list) { list->CopyBufferRegion(garbage.Get(), 0, upload.Get(), 0, garbage_size); });

        // Place a render target, a depth-stencil and a render target array over it. Nothing is submitted
        // between creation and the readback except what the layer does itself.
        ComPtr<ID3D12Resource> target = place(gpu, scratch.Get(), 0,
                                              tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 1, 2),
                                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12Resource> array = place(gpu, scratch.Get(), 1 * kMiB,
                                             tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 3, 1),
                                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12Resource> depth = place(gpu, scratch.Get(), 2 * kMiB,
                                             tex2d_desc(DXGI_FORMAT_D32_FLOAT, 32, 32, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
        Image image = gpu.read_texture(target.Get(), 0, 4);
        for (UINT y = 0; y < 64; y += 9) {
            for (UINT x = 0; x < 64; x += 7)
                expect_pixel("placed render target starts cleared", image.pixel(x, y), {0, 0, 0, 0}, 0);
        }
        image = gpu.read_texture(target.Get(), 1, 4);
        expect_pixel("placed render target, mip 1", image.pixel(3, 3), {0, 0, 0, 0}, 0);
        image = gpu.read_texture(array.Get(), 2, 4);  // the last slice
        expect_pixel("placed render target array, last slice", image.pixel(5, 5), {0, 0, 0, 0}, 0);
        image = gpu.read_texture(depth.Get(), 0, 4);
        float d;
        std::memcpy(&d, image.at(7, 7), 4);
        CHECK(d == 0.0f);
    }

    // ---- Allocation info ----------------------------------------------------------------------------------------------
    {
        const D3D12_RESOURCE_DESC descs[3] = {
            buffer_desc(100 * 1000),
            tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, D3D12_RESOURCE_FLAG_NONE, 1, 4),
            tex2d_desc(DXGI_FORMAT_BC7_UNORM, 512, 512, D3D12_RESOURCE_FLAG_NONE, 1, 10),
        };
        const D3D12_RESOURCE_ALLOCATION_INFO info = gpu.device->GetResourceAllocationInfo(0, 3, descs);
        CHECK(info.SizeInBytes != UINT64_MAX && info.SizeInBytes >= 100 * 1000 + 256 * 256 * 4 + 512 * 512);
        CHECK(info.Alignment >= 64 * 1024 && (info.Alignment & (info.Alignment - 1)) == 0);

        ComPtr<ID3D12Device4> device4;
        CHECK_HR(gpu.device.As(&device4));
        D3D12_RESOURCE_ALLOCATION_INFO1 info1[3];
        const D3D12_RESOURCE_ALLOCATION_INFO total = device4->GetResourceAllocationInfo1(0, 3, descs, info1);
        CHECK_EQ(total.SizeInBytes, info.SizeInBytes);
        CHECK_EQ(info1[0].Offset, 0u);
        CHECK(info1[1].Offset >= info1[0].SizeInBytes && info1[2].Offset >= info1[1].Offset + info1[1].SizeInBytes);
        CHECK(info1[2].Offset + info1[2].SizeInBytes <= total.SizeInBytes);

        // A heap of exactly that size holds all three at the reported offsets.
        ComPtr<ID3D12Heap> exact = create_heap(gpu, D3D12_HEAP_TYPE_DEFAULT, align_up(total.SizeInBytes, total.Alignment));
        for (int i = 0; i < 3; ++i)
            CHECK(place(gpu, exact.Get(), info1[i].Offset, descs[i]) != nullptr);

        // A single texture's size grows with its mips and array.
        const D3D12_RESOURCE_DESC small = tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64);
        const D3D12_RESOURCE_DESC big = tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, D3D12_RESOURCE_FLAG_NONE, 8, 7);
        CHECK(gpu.device->GetResourceAllocationInfo(0, 1, &big).SizeInBytes > gpu.device->GetResourceAllocationInfo(0, 1, &small).SizeInBytes);
    }

    // ---- Upload and readback heaps; residency; reserved resources ------------------------------------------------------
    {
        ComPtr<ID3D12Heap> upload_heap = create_heap(gpu, D3D12_HEAP_TYPE_UPLOAD, kMiB);
        ComPtr<ID3D12Resource> mapped = place(gpu, upload_heap.Get(), 64 * 1024, buffer_desc(4096), D3D12_RESOURCE_STATE_GENERIC_READ);
        void *pointer = nullptr;
        CHECK_HR(mapped->Map(0, nullptr, &pointer));
        std::memset(pointer, 0x5C, 4096);
        mapped->Unmap(0, nullptr);
        ComPtr<ID3D12Resource> target = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 4096);
        gpu.run([&](ID3D12GraphicsCommandList *list) { list->CopyBufferRegion(target.Get(), 0, mapped.Get(), 0, 4096); });
        const std::vector<uint8_t> bytes = gpu.read_buffer(target.Get(), 4096);
        CHECK(bytes[0] == 0x5C && bytes[4095] == 0x5C);

        ComPtr<ID3D12Heap> readback_heap = create_heap(gpu, D3D12_HEAP_TYPE_READBACK, kMiB);
        ComPtr<ID3D12Resource> readback = place(gpu, readback_heap.Get(), 0, buffer_desc(4096), D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.run([&](ID3D12GraphicsCommandList *list) { list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, 4096); });
        CHECK_HR(readback->Map(0, nullptr, &pointer));
        CHECK(static_cast<uint8_t *>(pointer)[100] == 0x5C);
        readback->Unmap(0, nullptr);

        ID3D12Pageable *pageables[] = {heap.Get(), target.Get()};
        CHECK_HR(gpu.device->MakeResident(2, pageables));
        CHECK_HR(gpu.device->Evict(2, pageables));
        CHECK_HR(gpu.device->MakeResident(2, pageables));
        ComPtr<ID3D12Device1> device1;
        CHECK_HR(gpu.device.As(&device1));
        const D3D12_RESIDENCY_PRIORITY priorities[2] = {D3D12_RESIDENCY_PRIORITY_NORMAL, D3D12_RESIDENCY_PRIORITY_HIGH};
        CHECK_HR(device1->SetResidencyPriority(2, pageables, priorities));

        ComPtr<ID3D12Resource> reserved;
        const D3D12_RESOURCE_DESC desc = buffer_desc(kMiB);
        CHECK(gpu.device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(reserved.GetAddressOf())) == E_NOTIMPL);
    }

    std::printf("p_heaps: PASS\n");
    return 0;
}
