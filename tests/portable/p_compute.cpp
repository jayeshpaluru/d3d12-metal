// Unordered access views and compute: a compute shader (tests/shaders/fill.hlsl) writes patterns into a UAV
// texture and buffers, uses a UAV counter, typed UAV loads and stores and raw byte address stores, with
// root constants and UAV barriers between dispatches; ClearUnorderedAccessView{Uint,Float} fills views.
#include <algorithm>
#include <cmath>
#include <set>

#include "fill_cs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 16;

struct Fixture {
    Gpu &gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> texture, structured, raw, appended, counter, float_texture, typed;
    static constexpr UINT64 kCounterOffset = 4096;

    explicit Fixture(Gpu &g) : gpu(g)
    {
        const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0);
        const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), descriptor_table(&range, 1)};
        signature = gpu.root_signature(parameters, 2, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_fill_cs));
        heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);

        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, uav));
        structured = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSize * kSize * 4, uav);
        raw = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSize * kSize * 4, uav);
        appended = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSize * kSize * 4, uav);
        counter = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kCounterOffset + 256, uav);
        float_texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R32_FLOAT, kSize, kSize, uav));
        typed = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSize * kSize * 4, uav);

        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 0));

        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = kSize * kSize;
        desc.Buffer.StructureByteStride = 4;
        gpu.device->CreateUnorderedAccessView(structured.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 1));

        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer.NumElements = kSize * kSize;
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        gpu.device->CreateUnorderedAccessView(raw.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 2));

        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = kSize * kSize;
        desc.Buffer.StructureByteStride = 4;
        desc.Buffer.CounterOffsetInBytes = kCounterOffset;
        gpu.device->CreateUnorderedAccessView(appended.Get(), counter.Get(), &desc, gpu.cpu_handle(heap.Get(), 3));

        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        gpu.device->CreateUnorderedAccessView(float_texture.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 4));

        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.Buffer.NumElements = kSize * kSize;
        gpu.device->CreateUnorderedAccessView(typed.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 5));
    }

    void dispatch(ID3D12GraphicsCommandList *list, UINT mode, UINT value, UINT groups_x = 2, UINT groups_y = 2)
    {
        const UINT constants[4] = {kSize, kSize, mode, value};
        list->SetComputeRoot32BitConstants(0, 4, constants, 0);
        list->Dispatch(groups_x, groups_y, 1);
    }

    ComPtr<ID3D12GraphicsCommandList> begin()
    {
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), 0));
        return list;
    }

    std::vector<uint32_t> words(ID3D12Resource *buffer, UINT64 offset = 0, UINT count = kSize * kSize)
    {
        const std::vector<uint8_t> bytes = gpu.read_buffer(buffer, count * 4ull, offset);
        std::vector<uint32_t> out(count);
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return out;
    }
};

} // namespace

int main()
{
    Gpu gpu;
    Fixture f(gpu);

    // ---- A pattern into a UAV texture and a UAV buffer ---------------------------------------------------
    {
        ComPtr<ID3D12GraphicsCommandList> list = f.begin();
        f.dispatch(list.Get(), 0, 0);
        gpu.run(list.Get());
        const Image image = gpu.read_texture(f.texture.Get(), 0, 4);
        for (UINT y = 0; y < kSize; ++y) {
            for (UINT x = 0; x < kSize; ++x)
                expect_pixel("UAV texture", image.pixel(x, y), {uint8_t(x), uint8_t(y), uint8_t(x ^ y), 255}, 1);
        }
        const std::vector<uint32_t> values = f.words(f.structured.Get());
        for (UINT y = 0; y < kSize; ++y) {
            for (UINT x = 0; x < kSize; ++x)
                CHECK_EQ(values[y * kSize + x], 0xA5000000u | (y << 8) | x);
        }
    }

    // ---- Two dispatches ordered by a UAV barrier, with different root constants ----------------------------
    {
        ComPtr<ID3D12GraphicsCommandList> list = f.begin();
        f.dispatch(list.Get(), 0, 0);
        const D3D12_RESOURCE_BARRIER barrier = uav_barrier(f.structured.Get());
        list->ResourceBarrier(1, &barrier);
        f.dispatch(list.Get(), 1, 5);
        list->ResourceBarrier(1, &barrier);
        f.dispatch(list.Get(), 1, 100);
        gpu.run(list.Get());
        const std::vector<uint32_t> values = f.words(f.structured.Get());
        for (UINT y = 0; y < kSize; ++y) {
            for (UINT x = 0; x < kSize; ++x)
                CHECK_EQ(values[y * kSize + x], (0xA5000000u | (y << 8) | x) + 105);
        }
    }

    // ---- A UAV counter ---------------------------------------------------------------------------------------
    {
        ComPtr<ID3D12GraphicsCommandList> list = f.begin();
        f.dispatch(list.Get(), 2, 0);
        gpu.run(list.Get());
        const std::vector<uint32_t> count = f.words(f.counter.Get(), Fixture::kCounterOffset, 1);
        CHECK_EQ(count[0], kSize * kSize);
        const std::vector<uint32_t> slots = f.words(f.appended.Get());
        std::set<uint32_t> seen(slots.begin(), slots.end());
        CHECK_EQ(seen.size(), size_t(kSize * kSize));
        CHECK_EQ(*seen.begin(), 1u);
        CHECK_EQ(*seen.rbegin(), kSize * kSize);
    }

    // ---- Typed UAV loads and stores ---------------------------------------------------------------------------
    {
        std::vector<float> floats(kSize * kSize);
        std::vector<uint32_t> ints(kSize * kSize);
        for (UINT i = 0; i < kSize * kSize; ++i) {
            floats[i] = i * 0.5f;
            ints[i] = 1000 + i;
        }
        ComPtr<ID3D12Resource> staging = gpu.upload_buffer(ints.data(), ints.size() * 4);
        gpu.upload_texture(f.float_texture.Get(), 0, floats.data(), 4);
        gpu.run([&](ID3D12GraphicsCommandList *l) { l->CopyBufferRegion(f.typed.Get(), 0, staging.Get(), 0, ints.size() * 4); });
        ComPtr<ID3D12GraphicsCommandList> list = f.begin();
        f.dispatch(list.Get(), 3, 7);
        gpu.run(list.Get());
        const Image image = gpu.read_texture(f.float_texture.Get(), 0, 4);
        for (UINT i = 0; i < kSize * kSize; ++i) {
            float v;
            std::memcpy(&v, image.at(i % kSize, i / kSize), 4);
            CHECK(std::fabs(v - (floats[i] * 2.0f + 1.0f)) < 1e-4f);
        }
        const std::vector<uint32_t> out = f.words(f.typed.Get());
        for (UINT i = 0; i < kSize * kSize; ++i)
            CHECK_EQ(out[i], ints[i] + 7);
    }

    // ---- Byte address buffer; group and thread ids -------------------------------------------------------------
    {
        ComPtr<ID3D12GraphicsCommandList> list = f.begin();
        f.dispatch(list.Get(), 4, 11);
        const D3D12_RESOURCE_BARRIER barrier = uav_barrier(nullptr);
        list->ResourceBarrier(1, &barrier);
        f.dispatch(list.Get(), 5, 0);
        gpu.run(list.Get());
        const std::vector<uint32_t> raw = f.words(f.raw.Get());
        for (UINT i = 0; i < kSize * kSize; ++i)
            CHECK_EQ(raw[i], i * 3 + 11);
        const std::vector<uint32_t> ids = f.words(f.structured.Get());
        for (UINT y = 0; y < kSize; ++y) {
            for (UINT x = 0; x < kSize; ++x)
                CHECK_EQ(ids[y * kSize + x], ((x / 8) << 16) | ((y / 8) << 8) | ((y % 8) * 8 + (x % 8)));
        }
    }

    // ---- ClearUnorderedAccessViewUint / Float --------------------------------------------------------------------
    {
        // Clears take the view from a CPU descriptor (a heap that is not shader visible) and the GPU handle of
        // the same view in the shader-visible heap.
        ComPtr<ID3D12DescriptorHeap> cpu_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, false);
        gpu.device->CopyDescriptorsSimple(8, cpu_heap->GetCPUDescriptorHandleForHeapStart(),
                                          f.heap->GetCPUDescriptorHandleForHeapStart(),
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ComPtr<ID3D12GraphicsCommandList> list = f.begin();
        const UINT words[4] = {0xDEADBEEFu, 0, 0, 0};
        list->ClearUnorderedAccessViewUint(gpu.gpu_handle(f.heap.Get(), 1), gpu.cpu_handle(cpu_heap.Get(), 1),
                                           f.structured.Get(), words, 0, nullptr);
        const UINT raw_words[4] = {0x01020304u, 0, 0, 0};
        list->ClearUnorderedAccessViewUint(gpu.gpu_handle(f.heap.Get(), 2), gpu.cpu_handle(cpu_heap.Get(), 2),
                                           f.raw.Get(), raw_words, 0, nullptr);
        const UINT typed_words[4] = {77, 0, 0, 0};
        list->ClearUnorderedAccessViewUint(gpu.gpu_handle(f.heap.Get(), 5), gpu.cpu_handle(cpu_heap.Get(), 5),
                                           f.typed.Get(), typed_words, 0, nullptr);
        const FLOAT colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        list->ClearUnorderedAccessViewFloat(gpu.gpu_handle(f.heap.Get(), 0), gpu.cpu_handle(cpu_heap.Get(), 0),
                                            f.texture.Get(), colour, 0, nullptr);
        const FLOAT value[4] = {3.5f, 0, 0, 0};
        list->ClearUnorderedAccessViewFloat(gpu.gpu_handle(f.heap.Get(), 4), gpu.cpu_handle(cpu_heap.Get(), 4),
                                            f.float_texture.Get(), value, 0, nullptr);
        gpu.run(list.Get());
        for (uint32_t v : f.words(f.structured.Get()))
            CHECK_EQ(v, 0xDEADBEEFu);
        for (uint32_t v : f.words(f.raw.Get()))
            CHECK_EQ(v, 0x01020304u);
        for (uint32_t v : f.words(f.typed.Get()))
            CHECK_EQ(v, 77u);
        Image image = gpu.read_texture(f.texture.Get(), 0, 4);
        expect_pixel("float clear of a UNORM texture", image.pixel(3, 9), {64, 128, 191, 255}, 1);
        image = gpu.read_texture(f.float_texture.Get(), 0, 4);
        float v;
        std::memcpy(&v, image.at(5, 5), 4);
        CHECK(v == 3.5f);

        // A rectangle: only that part of the texture changes.
        list = f.begin();
        const FLOAT green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        const D3D12_RECT rect = {2, 3, 8, 11};
        list->ClearUnorderedAccessViewFloat(gpu.gpu_handle(f.heap.Get(), 0), gpu.cpu_handle(cpu_heap.Get(), 0),
                                            f.texture.Get(), green, 1, &rect);
        gpu.run(list.Get());
        image = gpu.read_texture(f.texture.Get(), 0, 4);
        expect_pixel("inside the rectangle", image.pixel(2, 3), {0, 255, 0, 255}, 1);
        expect_pixel("inside the rectangle, far corner", image.pixel(7, 10), {0, 255, 0, 255}, 1);
        expect_pixel("outside the rectangle (right)", image.pixel(8, 5), {64, 128, 191, 255}, 1);
        expect_pixel("outside the rectangle (below)", image.pixel(4, 11), {64, 128, 191, 255}, 1);
        expect_pixel("outside the rectangle (left)", image.pixel(1, 5), {64, 128, 191, 255}, 1);
    }

    // ---- A dispatch with a zero group count does nothing -------------------------------------------------------------
    {
        ComPtr<ID3D12GraphicsCommandList> list = f.begin();
        f.dispatch(list.Get(), 5, 0, 0, 1);
        gpu.run(list.Get());
    }

    std::printf("p_compute: PASS\n");
    return 0;
}
