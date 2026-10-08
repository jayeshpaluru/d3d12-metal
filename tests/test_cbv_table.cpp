// Two draws whose colour comes from a constant buffer: the first through a
// descriptor table in a shader-visible heap, the second through a root CBV.
#include "render_context.h"

int main()
{
    RenderContext ctx;

    // Signature A: descriptor table with one CBV (b0).
    D3D12_DESCRIPTOR_RANGE1 range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER1 table_param = {};
    table_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table_param.DescriptorTable = {1, &range};
    table_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC1 table_desc = {};
    table_desc.NumParameters = 1;
    table_desc.pParameters = &table_param;
    table_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Com<ID3D12RootSignature> table_signature = ctx.create_root_signature(table_desc);

    // Signature B: root CBV (b0).
    D3D12_ROOT_PARAMETER1 cbv_param = {};
    cbv_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cbv_param.Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    cbv_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC1 cbv_desc = table_desc;
    cbv_desc.pParameters = &cbv_param;
    Com<ID3D12RootSignature> cbv_signature = ctx.create_root_signature(cbv_desc);

    Com<ID3D12PipelineState> table_pso = ctx.create_color_pso(table_signature.get());
    Com<ID3D12PipelineState> cbv_pso = ctx.create_color_pso(cbv_signature.get());

    // Two triangles, one in each half of the target.
    const float vertices[] = {
        -0.9f, -0.9f, 0.0f, -0.5f, 0.9f, 0.0f, -0.1f, -0.9f, 0.0f,  // left
        0.1f,  -0.9f, 0.0f, 0.5f,  0.9f, 0.0f, 0.9f,  -0.9f, 0.0f,  // right
    };
    Com<ID3D12Resource> vertex_buffer = ctx.create_upload_buffer(vertices, sizeof(vertices));
    D3D12_VERTEX_BUFFER_VIEW vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 3 * sizeof(float)};

    // Constant data: green at offset 0, magenta at offset 256 (CBV alignment).
    float constants[128] = {};
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    std::memcpy(constants, green, sizeof(green));
    std::memcpy(constants + 64, magenta, sizeof(magenta));
    Com<ID3D12Resource> constant_buffer = ctx.create_upload_buffer(constants, sizeof(constants));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 4;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Com<ID3D12DescriptorHeap> heap;
    CHECK_HR(ctx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(heap.put())));
    CHECK(heap->GetGPUDescriptorHandleForHeapStart().ptr != 0);
    // Put the CBV in the second slot so the table base is not the heap start.
    const UINT increment = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += increment;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {constant_buffer->GetGPUVirtualAddress(), 256};
    ctx.device->CreateConstantBufferView(&cbv, cpu);

    Com<ID3D12CommandAllocator> allocator;
    CHECK_HR(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
    Com<ID3D12GraphicsCommandList> list;
    CHECK_HR(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
                                           IID_PPV_ARGS(list.put())));

    const float clear_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.rtv();
    ID3D12DescriptorHeap *heaps[] = {heap.get()};
    list->SetDescriptorHeaps(1, heaps);
    ctx.set_viewport_and_scissor(list.get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &vbv);

    list->SetGraphicsRootSignature(table_signature.get());
    list->SetPipelineState(table_pso.get());
    list->SetGraphicsRootDescriptorTable(0, gpu);
    list->DrawInstanced(3, 1, 0, 0);

    list->SetGraphicsRootSignature(cbv_signature.get());
    list->SetPipelineState(cbv_pso.get());
    list->SetGraphicsRootConstantBufferView(0, constant_buffer->GetGPUVirtualAddress() + 256);
    list->DrawInstanced(3, 1, 3, 0);

    UINT row_pitch = 0;
    Com<ID3D12Resource> readback = ctx.record_readback(list.get(), &row_pitch);
    CHECK_HR(list->Close());
    ctx.execute_and_wait(list.get());

    check_pixel("left (descriptor table)", read_pixel(readback.get(), row_pitch, 16, 32), {0, 255, 0, 255});
    check_pixel("right (root CBV)", read_pixel(readback.get(), row_pitch, 48, 32), {255, 0, 255, 255});
    check_pixel("background", read_pixel(readback.get(), row_pitch, 32, 5), {0, 0, 255, 255});
    return 0;
}
