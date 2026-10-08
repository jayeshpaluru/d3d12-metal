// Synchronisation between the passes of one command list. Resources are not hazard tracked and shaders reach
// them through addresses and descriptor tables, so a missing barrier, fence or memory barrier shows up as
// stale data. Each sequence runs many times in one list; every result is checked.
#include "color_ps.h"
#include "color_vs.h"
#include "fill_cs.h"
#include "probe.h"

namespace {

constexpr int kRounds = 40;

// ---- copy -> sample --------------------------------------------------------------------------------------------

void copy_then_sample(Gpu &gpu)
{
    Probe probe(gpu);
    const D3D12_RESOURCE_DESC desc = tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64);
    ComPtr<ID3D12Resource> texture = gpu.texture(desc, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
    srv.Texture2D.MipLevels = 1;
    probe.set_srv(0, texture.Get(), srv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT rows;
    UINT64 row_size, total;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &total);

    for (int round = 0; round < kRounds; ++round) {
        ComPtr<ID3D12Resource> upload = gpu.buffer(D3D12_HEAP_TYPE_UPLOAD, total);
        uint8_t *mapped = nullptr;
        CHECK_HR(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
        for (UINT y = 0; y < 64; ++y) {
            for (UINT x = 0; x < 64; ++x) {
                uint8_t *p = mapped + y * footprint.Footprint.RowPitch + x * 4;
                p[0] = uint8_t(round * 5 + x);
                p[1] = uint8_t(y * 3);
                p[2] = uint8_t(round);
                p[3] = 255;
            }
        }
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        const D3D12_TEXTURE_COPY_LOCATION dst = subresource_location(texture.Get(), 0);
        const D3D12_TEXTURE_COPY_LOCATION src = footprint_location(upload.Get(), footprint);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const D3D12_RESOURCE_BARRIER to_srv = transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &to_srv);
        probe.record(list.Get(), 4, {{3, 7, 0, 0}, {60, 40, 0, 0}});
        const D3D12_RESOURCE_BARRIER back = transition(texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &back);
        gpu.run(list.Get());
        const std::vector<uint8_t> bytes = gpu.read_buffer(probe.results.Get(), 2 * sizeof(Float4));
        Float4 got[2];
        std::memcpy(got, bytes.data(), sizeof(got));
        expect_float4("copy then sample (1)", got[0], rgba8(uint8_t(round * 5 + 3), 21, round, 255), 0.005f);
        expect_float4("copy then sample (2)", got[1], rgba8(uint8_t(round * 5 + 60), 120, round, 255), 0.005f);
    }
}

// ---- compute (UAV) -> compute (SRV) -----------------------------------------------------------------------------------

struct FillFixture {
    Gpu &gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> texture, buffer, raw, appended, counter, float_texture, typed;

    explicit FillFixture(Gpu &g) : gpu(g)
    {
        const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0);
        const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), descriptor_table(&range, 1)};
        signature = gpu.root_signature(parameters, 2, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_fill_cs));
        heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);
        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, uav));
        buffer = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 16 * 16 * 4, uav);
        raw = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 16 * 16 * 4, uav);
        appended = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 16 * 16 * 4, uav);
        counter = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 8192, uav);
        float_texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R32_FLOAT, 16, 16, uav));
        typed = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 16 * 16 * 4, uav);
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 0));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 256;
        desc.Buffer.StructureByteStride = 4;
        gpu.device->CreateUnorderedAccessView(buffer.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 1));
        for (UINT slot : {3u}) {
            gpu.device->CreateUnorderedAccessView(appended.Get(), counter.Get(), &desc, gpu.cpu_handle(heap.Get(), slot));
        }
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer.NumElements = 256;
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        gpu.device->CreateUnorderedAccessView(raw.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 2));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        gpu.device->CreateUnorderedAccessView(float_texture.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 4));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.Buffer.NumElements = 256;
        gpu.device->CreateUnorderedAccessView(typed.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 5));
    }

    void begin(ID3D12GraphicsCommandList *list)
    {
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), 0));
    }

    void dispatch(ID3D12GraphicsCommandList *list, UINT mode, UINT value)
    {
        const UINT constants[4] = {16, 16, mode, value};
        list->SetComputeRoot32BitConstants(0, 4, constants, 0);
        list->Dispatch(2, 2, 1);
    }
};

void uav_then_srv(Gpu &gpu)
{
    FillFixture f(gpu);
    Probe probe(gpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC raw = srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R32_TYPELESS);
    raw.Buffer.NumElements = 256;
    raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    probe.set_srv(5, f.buffer.Get(), raw);

    for (int round = 0; round < kRounds; ++round) {
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        f.begin(list.Get());
        f.dispatch(list.Get(), 0, 0);                       // writes the buffer as a UAV
        const D3D12_RESOURCE_BARRIER barriers[2] = {
            uav_barrier(f.buffer.Get()),
            transition(f.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, barriers);
        probe.record(list.Get(), 6, {{0, 0, 0, 0}, {37, 0, 0, 0}, {252, 0, 0, 0}});  // reads it as an SRV
        const D3D12_RESOURCE_BARRIER back = transition(f.buffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &back);
        gpu.run(list.Get());
        const std::vector<uint8_t> bytes = gpu.read_buffer(probe.results.Get(), 3 * sizeof(Float4));
        uint32_t words[12];
        std::memcpy(words, bytes.data(), sizeof(words));
        // Element i of the pattern is 0xA5000000 | (y << 8) | x with i = y * 16 + x.
        CHECK_EQ(words[0], 0xA5000000u);
        CHECK_EQ(words[4], 0xA5000000u | (2u << 8) | 5u);   // element 37
        CHECK_EQ(words[8], 0xA5000000u | (15u << 8) | 12u);  // element 252
    }
}

// ---- compute -> graphics (a buffer written by a dispatch is the vertex buffer of the next draw) -------------------------

void compute_then_graphics(Gpu &gpu)
{
    FillFixture f(gpu);
    const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
    ComPtr<ID3D12RootSignature> graphics_signature = gpu.root_signature(&constants, 1);
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    ComPtr<ID3D12PipelineState> pso =
        gpu.graphics_pso(graphics_pso_desc(graphics_signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1));
    ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    for (int round = 0; round < kRounds; ++round) {
        const UINT scale = round % 2 ? 100 : 50;  // triangle at full size or half size
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        f.begin(list.Get());
        f.dispatch(list.Get(), 6, scale);
        const D3D12_RESOURCE_BARRIER barriers[2] = {
            uav_barrier(f.buffer.Get()),
            transition(f.buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)};
        list->ResourceBarrier(2, barriers);

        list->SetGraphicsRootSignature(graphics_signature.Get());
        list->SetPipelineState(pso.Get());
        const D3D12_VIEWPORT viewport = {0, 0, 32, 32, 0, 1};
        const D3D12_RECT scissor = {0, 0, 32, 32};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        const float colour[4] = {1, 0, 0, 1};
        list->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv = {f.buffer->GetGPUVirtualAddress(), 36, 12};
        list->IASetVertexBuffers(0, 1, &vbv);
        list->DrawInstanced(3, 1, 0, 0);
        const D3D12_RESOURCE_BARRIER back = transition(f.buffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &back);
        gpu.run(list.Get());
        const Image image = gpu.read_texture(target.Get(), 0, 4);
        // The triangle: bottom edge at y = -0.8 * scale, apex at 0.8 * scale. A pixel near the bottom middle
        // (NDC (0, -0.5)) is inside the full-size triangle and, at half size, outside it.
        expect_pixel("compute then graphics, apex area", image.pixel(16, 16), {255, 0, 0, 255});
        if (scale == 100)
            expect_pixel("full-size triangle", image.pixel(16, 24), {255, 0, 0, 255});
        else
            expect_pixel("half-size triangle", image.pixel(16, 24), {0, 0, 255, 255});
    }
}

// ---- render -> sample, in a different encoder type ------------------------------------------------------------------------

void render_then_sample(Gpu &gpu)
{
    Probe probe(gpu);
    const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&constants, 1);
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    ComPtr<ID3D12PipelineState> pso =
        gpu.graphics_pso(graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1));
    ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
    srv.Texture2D.MipLevels = 1;
    probe.set_srv(0, target.Get(), srv);
    const float triangle[] = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
    ComPtr<ID3D12Resource> vertices = gpu.upload_buffer(triangle, sizeof(triangle));
    const D3D12_VERTEX_BUFFER_VIEW vbv = {vertices->GetGPUVirtualAddress(), sizeof(triangle), 12};

    for (int round = 0; round < kRounds; ++round) {
        const float colour[4] = {round / 40.0f, 1.0f - round / 40.0f, 0.5f, 1};
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        list->SetGraphicsRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        const D3D12_VIEWPORT viewport = {0, 0, 16, 16, 0, 1};
        const D3D12_RECT scissor = {0, 0, 16, 16};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {0, 0, 0, 1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->DrawInstanced(3, 1, 0, 0);
        const D3D12_RESOURCE_BARRIER to_srv = transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &to_srv);
        probe.record(list.Get(), 4, {{4, 4, 0, 0}});
        const D3D12_RESOURCE_BARRIER back = transition(target.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &back);
        gpu.run(list.Get());
        const std::vector<uint8_t> bytes = gpu.read_buffer(probe.results.Get(), sizeof(Float4));
        Float4 got;
        std::memcpy(&got, bytes.data(), sizeof(got));
        expect_float4("render then sample", got, {colour[0], colour[1], colour[2], 1}, 0.01f);
    }
}

// ---- Independent work overlaps; a barrier orders it ------------------------------------------------------------------------

void independent_dispatches(Gpu &gpu)
{
    FillFixture f(gpu);
    ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
    f.begin(list.Get());
    // Three dispatches on different resources with no barrier between them, then a barrier and a dispatch that uses all.
    f.dispatch(list.Get(), 4, 11);   // raw buffer
    f.dispatch(list.Get(), 0, 0);    // texture and structured buffer
    f.dispatch(list.Get(), 5, 0);    // structured buffer again, after mode 0 (same resource, no barrier: undefined order is the app's problem)
    const D3D12_RESOURCE_BARRIER barrier = uav_barrier(nullptr);
    list->ResourceBarrier(1, &barrier);
    f.dispatch(list.Get(), 4, 12);   // overwrites the raw buffer after the barrier
    gpu.run(list.Get());
    const std::vector<uint8_t> bytes = gpu.read_buffer(f.raw.Get(), 256 * 4);
    uint32_t words[256];
    std::memcpy(words, bytes.data(), sizeof(words));
    for (UINT i = 0; i < 256; ++i)
        CHECK_EQ(words[i], i * 3 + 12);
}

// ---- Aliasing: one resource written, another over the same memory read after an aliasing barrier ----------------------

void aliasing(Gpu &gpu)
{
    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = 1024 * 1024;
    heap_desc.Properties = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    ComPtr<ID3D12Heap> heap;
    CHECK_HR(gpu.device->CreateHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf())));
    const D3D12_RESOURCE_DESC desc = buffer_desc(1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> a, b;
    CHECK_HR(gpu.device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(a.GetAddressOf())));
    CHECK_HR(gpu.device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(b.GetAddressOf())));
    for (int round = 0; round < kRounds; ++round) {
        std::vector<uint32_t> data(256, 0x77000000u + round);
        ComPtr<ID3D12Resource> upload = gpu.upload_buffer(data.data(), 1024);
        ComPtr<ID3D12Resource> readback = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 1024);
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        list->CopyBufferRegion(a.Get(), 0, upload.Get(), 0, 1024);
        const D3D12_RESOURCE_BARRIER barrier = aliasing_barrier(a.Get(), b.Get());
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback.Get(), 0, b.Get(), 0, 1024);
        gpu.run(list.Get());
        void *mapped = nullptr;
        CHECK_HR(readback->Map(0, nullptr, &mapped));
        CHECK(std::memcmp(mapped, data.data(), 1024) == 0);
        readback->Unmap(0, nullptr);
    }
}

} // namespace

int main()
{
    Gpu gpu;
    copy_then_sample(gpu);
    uav_then_srv(gpu);
    compute_then_graphics(gpu);
    render_then_sample(gpu);
    independent_dispatches(gpu);
    aliasing(gpu);
    std::printf("p_barriers: PASS\n");
    return 0;
}
