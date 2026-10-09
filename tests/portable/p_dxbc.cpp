// SPDX-License-Identifier: LGPL-2.1-or-later
// Shader Model 5 bytecode (DXBC, as FXC and D3DCompile produce it) is accepted: tests/shaders/dxbc.hlsl is converted
// to DXIL by dxilconv and runs as a draw and as a compute dispatch. Needs libdxilconv (tools/build-dxilconv.sh).
#include "dxbc_cs.h"
#include "dxbc_ps.h"
#include "dxbc_vs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 16;

} // namespace

int main()
{
    Gpu gpu;

    // ---- A triangle covering the target, coloured by a root constant -----------------------------------------------
    {
        const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
        ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&constants, 1);
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(
            graphics_pso_desc(signature.Get(), T12_SHADER(g_dxbc_vs), T12_SHADER(g_dxbc_ps), layout, 1));

        const float vertices[9] = {-1, -1, 0.5f, -1, 3, 0.5f, 3, -1, 0.5f};
        ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(vertices, sizeof(vertices));
        const D3D12_VERTEX_BUFFER_VIEW view = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 12};

        ComPtr<ID3D12Resource> target = gpu.texture(
            tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->SetGraphicsRootSignature(signature.Get());
            list->SetPipelineState(pso.Get());
            const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const float clear[4] = {0, 0, 0, 1};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &view);
            const float colour[4] = {0.25f, 0.5f, 1.0f, 1.0f};
            list->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
            list->DrawInstanced(3, 1, 0, 0);
        });
        const Image image = gpu.read_texture(target.Get(), 0, 4);
        expect_pixel("DXBC pixel shader", image.pixel(kSize / 2, kSize / 2), {64, 128, 255, 255}, 2);
        expect_pixel("DXBC pixel shader corner", image.pixel(1, 1), {64, 128, 255, 255}, 2);
    }

    // ---- A compute shader with a loop and a UAV -----------------------------------------------------------------------
    {
        const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), descriptor_table(&range, 1)};
        ComPtr<ID3D12RootSignature> signature =
            gpu.root_signature(parameters, 2, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3D12PipelineState> pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_dxbc_cs));

        ComPtr<ID3D12Resource> buffer = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 8 * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12DescriptorHeap> heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 8;
        desc.Buffer.StructureByteStride = 4;
        gpu.device->CreateUnorderedAccessView(buffer.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 0));

        gpu.run([&](ID3D12GraphicsCommandList *list) {
            ID3D12DescriptorHeap *heaps[] = {heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(signature.Get());
            list->SetPipelineState(pso.Get());
            const float zero[4] = {0, 0, 0, 0};  // asuint(0.0f) == 0
            list->SetComputeRoot32BitConstants(0, 4, zero, 0);
            list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), 0));
            list->Dispatch(1, 1, 1);
        });
        const std::vector<uint8_t> bytes = gpu.read_buffer(buffer.Get(), 8 * 4);
        for (UINT x = 0; x < 8; ++x) {
            uint32_t value;
            std::memcpy(&value, bytes.data() + 4 * x, 4);
            uint32_t expected = 0;
            for (UINT i = 0; i <= x; ++i)
                expected += i * 3 + 1;
            CHECK_EQ(value, expected);
        }
    }

    return 0;
}
