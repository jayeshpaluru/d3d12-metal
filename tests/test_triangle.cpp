// Renders a triangle whose colour comes from four root constants, then reads
// the render target back.
#include "render_context.h"

int main()
{
    RenderContext ctx;

    // Root signature: four 32-bit constants (the colour) at b0 for the pixel shader.
    D3D12_ROOT_PARAMETER1 param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {0, 0, 4};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC1 rs_desc = {};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &param;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Com<ID3D12RootSignature> signature = ctx.create_root_signature(rs_desc);
    Com<ID3D12PipelineState> pso = ctx.create_color_pso(signature.get());

    const float vertices[] = {-0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f};
    Com<ID3D12Resource> vertex_buffer = ctx.create_upload_buffer(vertices, sizeof(vertices));
    D3D12_VERTEX_BUFFER_VIEW vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 3 * sizeof(float)};

    Com<ID3D12CommandAllocator> allocator;
    CHECK_HR(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
    Com<ID3D12GraphicsCommandList> list;
    CHECK_HR(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), pso.get(),
                                           IID_PPV_ARGS(list.put())));

    const float clear_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const float triangle_color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.rtv();
    list->SetGraphicsRootSignature(signature.get());
    ctx.set_viewport_and_scissor(list.get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &vbv);
    list->SetGraphicsRoot32BitConstants(0, 4, triangle_color, 0);
    list->DrawInstanced(3, 1, 0, 0);

    UINT row_pitch = 0;
    Com<ID3D12Resource> readback = ctx.record_readback(list.get(), &row_pitch);
    CHECK_HR(list->Close());
    ctx.execute_and_wait(list.get());

    check_pixel("center", read_pixel(readback.get(), row_pitch, 32, 32), {255, 0, 0, 255});
    check_pixel("corner", read_pixel(readback.get(), row_pitch, 0, 0), {0, 0, 255, 255});
    return 0;
}
