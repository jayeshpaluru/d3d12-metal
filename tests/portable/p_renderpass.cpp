// SPDX-License-Identifier: LGPL-2.1-or-later
// Render passes (ID3D12GraphicsCommandList4): clears by beginning access, depth, resolves at the end, and a pass
// split into a suspending and a resuming one across two lists.
#include "color_ps.h"
#include "color_vs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 32;
constexpr Pixel kBlue = {0, 0, 255, 255}, kRed = {255, 0, 0, 255}, kGreen = {0, 255, 0, 255};

D3D12_RENDER_PASS_BEGINNING_ACCESS clear_access(float r, float g, float b, float a)
{
    D3D12_RENDER_PASS_BEGINNING_ACCESS access = {};
    access.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
    access.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    access.Clear.ClearValue.Color[0] = r;
    access.Clear.ClearValue.Color[1] = g;
    access.Clear.ClearValue.Color[2] = b;
    access.Clear.ClearValue.Color[3] = a;
    return access;
}

D3D12_RENDER_PASS_BEGINNING_ACCESS plain_begin(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE type)
{
    D3D12_RENDER_PASS_BEGINNING_ACCESS access = {};
    access.Type = type;
    return access;
}

D3D12_RENDER_PASS_ENDING_ACCESS plain_end(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE type)
{
    D3D12_RENDER_PASS_ENDING_ACCESS access = {};
    access.Type = type;
    return access;
}

} // namespace

int main()
{
    Gpu gpu;
    const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&constants, 1);
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC depth_desc =
        graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1);
    depth_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    depth_desc.DepthStencilState.DepthEnable = TRUE;
    depth_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ComPtr<ID3D12PipelineState> depth_pso = gpu.graphics_pso(depth_desc);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC msaa_desc =
        graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1);
    msaa_desc.SampleDesc.Count = 4;
    ComPtr<ID3D12PipelineState> msaa_pso = gpu.graphics_pso(msaa_desc);

    ComPtr<ID3D12PipelineState> plain_pso =
        gpu.graphics_pso(graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1));

    // A big triangle covering the middle of the target at depth z, and a small one in the upper right.
    auto triangle = [&](float z) {
        const float v[9] = {-0.9f, -0.9f, z, 0.9f, -0.9f, z, 0.0f, 0.9f, z};
        return gpu.upload_buffer(v, sizeof(v));
    };
    ComPtr<ID3D12Resource> near_triangle = triangle(0.3f), far_triangle = triangle(0.8f);
    const float corner[9] = {0.5f, 0.5f, 0.1f, 0.9f, 0.5f, 0.1f, 0.9f, 0.9f, 0.1f};
    ComPtr<ID3D12Resource> corner_triangle = gpu.upload_buffer(corner, sizeof(corner));
    auto view = [](ID3D12Resource *buffer) { return D3D12_VERTEX_BUFFER_VIEW{buffer->GetGPUVirtualAddress(), 36, 12}; };

    auto setup = [&](ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso) {
        list->SetGraphicsRootSignature(signature.Get());
        list->SetPipelineState(pso);
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    auto draw = [&](ID3D12GraphicsCommandList *list, ID3D12Resource *vertices, const float colour[4]) {
        D3D12_VERTEX_BUFFER_VIEW v = view(vertices);
        list->IASetVertexBuffers(0, 1, &v);
        list->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
        list->DrawInstanced(3, 1, 0, 0);
    };
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};

    // ---- Clear and depth by beginning access ---------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12Resource> depth = gpu.texture(tex2d_desc(DXGI_FORMAT_D32_FLOAT, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        ComPtr<ID3D12DescriptorHeap> dsv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        gpu.device->CreateDepthStencilView(depth.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());

        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        ComPtr<ID3D12GraphicsCommandList4> list4;
        CHECK_HR(list.As(&list4));
        setup(list.Get(), depth_pso.Get());
        D3D12_RENDER_PASS_RENDER_TARGET_DESC rt = {};
        rt.cpuDescriptor = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rt.BeginningAccess = clear_access(0, 0, 1, 1);
        rt.EndingAccess = plain_end(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds = {};
        ds.cpuDescriptor = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        ds.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        ds.DepthBeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_D32_FLOAT;
        ds.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = 1.0f;
        ds.StencilBeginningAccess = plain_begin(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS);
        ds.DepthEndingAccess = plain_end(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD);
        ds.StencilEndingAccess = plain_end(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS);
        list4->BeginRenderPass(1, &rt, &ds, D3D12_RENDER_PASS_FLAG_NONE);
        draw(list.Get(), near_triangle.Get(), red);
        draw(list.Get(), far_triangle.Get(), green);  // behind the first: the depth test, cleared by the pass, hides it
        draw(list.Get(), corner_triangle.Get(), green);
        list4->EndRenderPass();
        gpu.run(list.Get());
        const Image image = gpu.read_texture(target.Get(), 0, 4);
        expect_pixel("pass: cleared colour", image.pixel(1, 1), kBlue, 0);
        expect_pixel("pass: near triangle", image.pixel(16, 20), kRed, 0);
        expect_pixel("pass: far triangle hidden by depth", image.pixel(16, 28), kRed, 0);
        expect_pixel("pass: nearer small triangle", image.pixel(28, 6), kGreen, 0);
    }

    // ---- A multisampled target resolved by the ending access ---------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> msaa = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 1, 1, 4),
                                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12Resource> resolved = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                      D3D12_RESOURCE_STATE_RESOLVE_DEST);
        ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        gpu.device->CreateRenderTargetView(msaa.Get(), &rtv_desc, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        ComPtr<ID3D12GraphicsCommandList4> list4;
        CHECK_HR(list.As(&list4));
        setup(list.Get(), msaa_pso.Get());
        D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS sub = {};
        sub.SrcRect = {0, 0, LONG(kSize), LONG(kSize)};
        D3D12_RENDER_PASS_RENDER_TARGET_DESC rt = {};
        rt.cpuDescriptor = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rt.BeginningAccess = clear_access(0, 0, 1, 1);
        rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
        rt.EndingAccess.Resolve.pSrcResource = msaa.Get();
        rt.EndingAccess.Resolve.pDstResource = resolved.Get();
        rt.EndingAccess.Resolve.SubresourceCount = 1;
        rt.EndingAccess.Resolve.pSubresourceParameters = &sub;
        rt.EndingAccess.Resolve.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt.EndingAccess.Resolve.ResolveMode = D3D12_RESOLVE_MODE_AVERAGE;
        list4->BeginRenderPass(1, &rt, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        draw(list.Get(), near_triangle.Get(), red);
        list4->EndRenderPass();
        gpu.run(list.Get());
        const Image image = gpu.read_texture(resolved.Get(), 0, 4);
        expect_pixel("resolve at pass end: cleared colour", image.pixel(1, 1), kBlue, 0);
        expect_pixel("resolve at pass end: triangle", image.pixel(16, 20), kRed, 0);
    }

    // ---- A pass suspended in one list and resumed in the next ---------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_RENDER_PASS_RENDER_TARGET_DESC rt = {};
        rt.cpuDescriptor = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rt.BeginningAccess = clear_access(0, 0, 1, 1);
        rt.EndingAccess = plain_end(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);

        ComPtr<ID3D12GraphicsCommandList> first = gpu.list(), second = gpu.list();
        ComPtr<ID3D12GraphicsCommandList4> first4, second4;
        CHECK_HR(first.As(&first4));
        CHECK_HR(second.As(&second4));
        setup(first.Get(), plain_pso.Get());
        first4->BeginRenderPass(1, &rt, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
        draw(first.Get(), near_triangle.Get(), red);
        first4->EndRenderPass();
        CHECK_HR(first->Close());
        setup(second.Get(), plain_pso.Get());
        second4->BeginRenderPass(1, &rt, nullptr, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);  // the clear does not apply again
        draw(second.Get(), corner_triangle.Get(), green);
        second4->EndRenderPass();
        CHECK_HR(second->Close());
        ID3D12CommandList *lists[] = {first.Get(), second.Get()};
        gpu.queue->ExecuteCommandLists(2, lists);
        gpu.wait_idle();
        const Image image = gpu.read_texture(target.Get(), 0, 4);
        expect_pixel("resumed pass keeps the first part", image.pixel(16, 20), kRed, 0);
        expect_pixel("resumed pass adds the second part", image.pixel(28, 6), kGreen, 0);
        expect_pixel("resumed pass keeps the clear", image.pixel(1, 1), kBlue, 0);
    }

    // ---- Calls out of order are ignored -----------------------------------------------------------------------------------
    {
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        ComPtr<ID3D12GraphicsCommandList4> list4;
        CHECK_HR(list.As(&list4));
        list4->EndRenderPass();
        CHECK_HR(list->Close());
    }

    std::printf("p_renderpass: PASS\n");
    return 0;
}
