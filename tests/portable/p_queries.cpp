// Queries (occlusion, binary occlusion, timestamps, pipeline statistics), markers and events, predication,
// WriteBufferImmediate and clock calibration.
#include "color_ps.h"
#include "color_vs.h"
#include "fill_cs.h"
#include "t12.h"

#include <chrono>
#include <thread>

namespace {

constexpr UINT kSize = 64;


uint64_t read_u64(const std::vector<uint8_t> &bytes, size_t index)
{
    uint64_t v;
    std::memcpy(&v, bytes.data() + index * 8, 8);
    return v;
}

} // namespace

int main()
{
    Gpu gpu;

    // ---- Occlusion queries ---------------------------------------------------------------------------------------------
    {
        const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
        ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&constants, 1);
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc =
            graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1);
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(desc);

        ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12Resource> depth = gpu.texture(tex2d_desc(DXGI_FORMAT_D32_FLOAT, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        ComPtr<ID3D12DescriptorHeap> dsv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        gpu.device->CreateDepthStencilView(depth.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());

        auto make = [&](float z, float scale) {
            const float v[9] = {-0.5f * scale, -0.5f * scale, z, 0.5f * scale, -0.5f * scale, z, 0.0f, 0.5f * scale, z};
            return gpu.upload_buffer(v, sizeof(v));
        };
        ComPtr<ID3D12Resource> near_big = make(0.2f, 1.6f), visible = make(0.1f, 0.8f), hidden = make(0.9f, 0.8f), nearer = make(0.05f, 0.8f);
        auto view = [](ID3D12Resource *buffer) { return D3D12_VERTEX_BUFFER_VIEW{buffer->GetGPUVirtualAddress(), 36, 12}; };

        D3D12_QUERY_HEAP_DESC heap_desc = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, 4, 0};
        ComPtr<ID3D12QueryHeap> heap;
        CHECK_HR(gpu.device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf())));
        ComPtr<ID3D12Resource> results = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 4 * 8);

        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->SetGraphicsRootSignature(signature.Get());
            list->SetPipelineState(pso.Get());
            const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            const float clear[4] = {0, 0, 0, 1};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const float grey[4] = {0.5f, 0.5f, 0.5f, 1}, green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1};
            // A big near triangle first, then queries around draws that pass and fail the depth test.
            list->SetGraphicsRoot32BitConstants(0, 4, grey, 0);
            D3D12_VERTEX_BUFFER_VIEW v = view(near_big.Get());
            list->IASetVertexBuffers(0, 1, &v);
            list->DrawInstanced(3, 1, 0, 0);
            list->SetGraphicsRoot32BitConstants(0, 4, green, 0);
            v = view(visible.Get());
            list->IASetVertexBuffers(0, 1, &v);
            list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
            list->DrawInstanced(3, 1, 0, 0);
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
            list->SetGraphicsRoot32BitConstants(0, 4, red, 0);
            v = view(hidden.Get());
            list->IASetVertexBuffers(0, 1, &v);
            list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
            list->DrawInstanced(3, 1, 0, 0);
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
            list->SetGraphicsRoot32BitConstants(0, 4, green, 0);
            v = view(nearer.Get());  // nearer than the visible one: passes the depth test again, over the same pixels
            list->IASetVertexBuffers(0, 1, &v);
            list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2);
            list->DrawInstanced(3, 1, 0, 0);
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2);
            list->SetGraphicsRoot32BitConstants(0, 4, red, 0);
            v = view(hidden.Get());
            list->IASetVertexBuffers(0, 1, &v);
            list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 3);
            list->DrawInstanced(3, 1, 0, 0);
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 3);
            list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 4, results.Get(), 0);
        });
        // The visible triangle's pixels: the green ones in the target.
        const Image image = gpu.read_texture(target.Get(), 0, 4);
        UINT green_pixels = 0;
        for (UINT y = 0; y < kSize; ++y) {
            for (UINT x = 0; x < kSize; ++x) {
                const Pixel p = image.pixel(x, y);
                green_pixels += (p.g == 255 && p.r == 0);
            }
        }
        CHECK(green_pixels > 300);
        uint8_t *mapped = nullptr;
        CHECK_HR(results->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
        std::vector<uint8_t> bytes(mapped, mapped + 32);
        results->Unmap(0, nullptr);
        CHECK_EQ(read_u64(bytes, 0), green_pixels);
        CHECK_EQ(read_u64(bytes, 1), 0u);
        CHECK_EQ(read_u64(bytes, 2), 1u);
        CHECK_EQ(read_u64(bytes, 3), 0u);

        // A query that spans two render passes (a depth clear between the draws ends the first) counts both.
        ComPtr<ID3D12Resource> spanned = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 8);
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->SetGraphicsRootSignature(signature.Get());
            list->SetPipelineState(pso.Get());
            const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const float green[4] = {0, 1, 0, 1};
            list->SetGraphicsRoot32BitConstants(0, 4, green, 0);
            D3D12_VERTEX_BUFFER_VIEW v = view(visible.Get());
            list->IASetVertexBuffers(0, 1, &v);
            list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
            list->DrawInstanced(3, 1, 0, 0);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
            list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1, 1, spanned.Get(), 0);
        });
        CHECK_HR(spanned->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
        std::vector<uint8_t> span_bytes(mapped, mapped + 8);
        spanned->Unmap(0, nullptr);
        CHECK_EQ(read_u64(span_bytes, 0), 2u * green_pixels);
    }

    // ---- Timestamps, pipeline statistics, clock calibration ----------------------------------------------------------------
    {
        UINT64 frequency = 0;
        CHECK_HR(gpu.queue->GetTimestampFrequency(&frequency));
        CHECK(frequency >= 1000000);
        D3D12_QUERY_HEAP_DESC heap_desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
        ComPtr<ID3D12QueryHeap> heap;
        CHECK_HR(gpu.device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf())));
        ComPtr<ID3D12Resource> results = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 4 * 8);

        // A buffer clear of some size between two timestamps.
        ComPtr<ID3D12Resource> big = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 64 * 1024 * 1024);
        ComPtr<ID3D12Resource> big2 = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 64 * 1024 * 1024);
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            for (int i = 0; i < 8; ++i) {
                list->CopyBufferRegion(big2.Get(), 0, big.Get(), 0, 64 * 1024 * 1024);
                const D3D12_RESOURCE_BARRIER barrier = uav_barrier(nullptr);
                list->ResourceBarrier(1, &barrier);
            }
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
            list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, results.Get(), 0);
        });
        uint8_t *mapped = nullptr;
        CHECK_HR(results->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
        std::vector<uint8_t> bytes(mapped, mapped + 32);
        results->Unmap(0, nullptr);
        const uint64_t t0 = read_u64(bytes, 0), t1 = read_u64(bytes, 1);
        std::printf("timestamps: %llu -> %llu (%.3f ms)\n", static_cast<unsigned long long>(t0), static_cast<unsigned long long>(t1),
                    double(t1 - t0) * 1000.0 / double(frequency));
        CHECK(t0 != 0 && t1 > t0);
        CHECK(double(t1 - t0) / double(frequency) < 5.0);  // seconds

        // A timestamp resolve does not block the queue: a wait for a fence that another thread signals later,
        // encoded ahead of it in the same command buffer, must not deadlock the submit.
        {
            ComPtr<ID3D12Fence> late;
            CHECK_HR(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(late.GetAddressOf())));
            ComPtr<ID3D12Resource> results2 = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 4 * 8);
            ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
            list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3, 1, results2.Get(), 0);
            CHECK_HR(list->Close());
            CHECK_HR(gpu.queue->Wait(late.Get(), 1));
            gpu.execute(list.Get());
            std::thread signaller([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                late->Signal(1);
            });
            gpu.wait_idle();
            signaller.join();
            void *mapped2 = nullptr;
            CHECK_HR(results2->Map(0, nullptr, &mapped2));
            uint64_t late_stamp;
            std::memcpy(&late_stamp, mapped2, 8);
            results2->Unmap(0, nullptr);
            CHECK(late_stamp > t1);
        }

        UINT64 gpu_clock = 0, cpu_clock = 0;
        CHECK_HR(gpu.queue->GetClockCalibration(&gpu_clock, &cpu_clock));
        CHECK(gpu_clock != 0 && cpu_clock != 0);
        // The GPU clock of the calibration is later than the timestamps taken before it.
        CHECK(gpu_clock >= t1);

        // Pipeline statistics: no data, zeros of the right size.
        D3D12_QUERY_HEAP_DESC stats_desc = {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2, 0};
        ComPtr<ID3D12QueryHeap> stats;
        CHECK_HR(gpu.device->CreateQueryHeap(&stats_desc, IID_PPV_ARGS(stats.GetAddressOf())));
        ComPtr<ID3D12Resource> stats_results = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 2 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        CHECK_HR(stats_results->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
        std::memset(mapped, 0xff, 2 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        stats_results->Unmap(0, nullptr);
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->BeginQuery(stats.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            list->EndQuery(stats.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            list->ResolveQueryData(stats.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 2, stats_results.Get(), 0);
        });
        CHECK_HR(stats_results->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
        for (size_t i = 0; i < 2 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS); ++i)
            CHECK_EQ(mapped[i], 0);
        stats_results->Unmap(0, nullptr);

        // A query of the wrong type for its heap is refused (the call is ignored).
        gpu.run([&](ID3D12GraphicsCommandList *list) { list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0); });
    }

    // ---- Markers, events, predication, sample positions ------------------------------------------------------------------------
    {
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            const char text[] = "ansi event";
            list->BeginEvent(1, text, sizeof(text));
            list->SetMarker(1, "a marker", 9);
            const wchar_t wide[] = L"unicode event";
            list->BeginEvent(0, wide, sizeof(wide));
            list->EndEvent();
            list->EndEvent();
            list->EndEvent();  // unbalanced: ignored
            list->SetMarker(2, "pix blob", 9);
            list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        });
        const char queue_text[] = "queue event";
        gpu.queue->BeginEvent(1, queue_text, sizeof(queue_text));
        gpu.queue->SetMarker(1, "queue marker", 13);
        gpu.queue->EndEvent();
        gpu.queue->EndEvent();
        gpu.wait_idle();
    }

    // ---- WriteBufferImmediate -------------------------------------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> buffer = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> readback = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 64);
        std::vector<uint32_t> data(16, 0x11111111u);
        ComPtr<ID3D12Resource> upload = gpu.upload_buffer(data.data(), 64);
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        ComPtr<ID3D12GraphicsCommandList2> list2;
        CHECK_HR(list.As(&list2));
        list->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, 64);
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[3] = {{buffer->GetGPUVirtualAddress() + 4, 0xAAAA0001u},
                                                          {buffer->GetGPUVirtualAddress() + 20, 0xBBBB0002u},
                                                          {buffer->GetGPUVirtualAddress() + 60, 0xCCCC0003u}};
        D3D12_WRITEBUFFERIMMEDIATE_MODE modes[3] = {D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
                                                    D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT};
        list2->WriteBufferImmediate(3, params, modes);
        const D3D12_RESOURCE_BARRIER barrier = uav_barrier(buffer.Get());
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback.Get(), 0, buffer.Get(), 0, 64);
        gpu.run(list.Get());
        uint32_t words[16];
        void *mapped = nullptr;
        CHECK_HR(readback->Map(0, nullptr, &mapped));
        std::memcpy(words, mapped, 64);
        readback->Unmap(0, nullptr);
        CHECK_EQ(words[0], 0x11111111u);
        CHECK_EQ(words[1], 0xAAAA0001u);
        CHECK_EQ(words[5], 0xBBBB0002u);
        CHECK_EQ(words[15], 0xCCCC0003u);
        CHECK_EQ(words[14], 0x11111111u);

        // A destination that is not 4-byte aligned is refused and nothing is written.
        ComPtr<ID3D12GraphicsCommandList> again = gpu.list();
        ComPtr<ID3D12GraphicsCommandList2> again2;
        CHECK_HR(again.As(&again2));
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER odd[2] = {{buffer->GetGPUVirtualAddress() + 10, 0xDEADBEEFu},
                                                             {buffer->GetGPUVirtualAddress() + 8, 0x0BADF00Du}};
        again2->WriteBufferImmediate(2, odd, nullptr);
        again->CopyBufferRegion(readback.Get(), 0, buffer.Get(), 0, 64);
        gpu.run(again.Get());
        CHECK_HR(readback->Map(0, nullptr, &mapped));
        std::memcpy(words, mapped, 64);
        readback->Unmap(0, nullptr);
        CHECK_EQ(words[2], 0x0BADF00Du);
        CHECK_EQ(words[3], 0x11111111u);
    }

    std::printf("p_queries: PASS\n");
    return 0;
}
