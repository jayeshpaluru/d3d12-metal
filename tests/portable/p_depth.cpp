// SPDX-License-Identifier: LGPL-2.1-or-later
// Depth and stencil: DSVs, ClearDepthStencilView, depth testing and writing, stencil operations, depth-only
// passes, depth planes copied to buffers (planar footprints), a depth texture read through an SRV, D24S8, and
// descriptor increment sizes per heap type.
#include "color_ps.h"
#include "color_vs.h"
#include "probe.h"

namespace {

constexpr UINT kSize = 64;

struct Triangle {
    ComPtr<ID3D12Resource> buffer;
    D3D12_VERTEX_BUFFER_VIEW view;
};

// Three float3 positions. `big` covers the whole target, the small one only its middle.
Triangle make_triangle(Gpu &gpu, bool big, float z)
{
    const float big_vertices[] = {-1, -1, z, 3, -1, z, -1, 3, z};
    const float small_vertices[] = {-0.5f, -0.5f, z, 0.5f, -0.5f, z, 0.0f, 0.5f, z};
    Triangle t;
    t.buffer = gpu.upload_buffer(big ? static_cast<const void *>(big_vertices) : small_vertices, sizeof(big_vertices));
    t.view = {t.buffer->GetGPUVirtualAddress(), sizeof(big_vertices), 3 * sizeof(float)};
    return t;
}

struct Draw {
    const Triangle *triangle;
    float color[4];
    ID3D12PipelineState *pso;
    UINT stencil_ref = 0;
};

struct Fixture {
    Gpu &gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12Resource> target, depth;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    DXGI_FORMAT dsv_format;

    Fixture(Gpu &g, DXGI_FORMAT resource_format, DXGI_FORMAT view_format) : gpu(g), dsv_format(view_format)
    {
        const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
        signature = gpu.root_signature(&constants, 1);
        target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        depth = gpu.texture(tex2d_desc(resource_format, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        dsv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = view_format;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        gpu.device->CreateDepthStencilView(depth.Get(), &dsv, dsv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    ComPtr<ID3D12PipelineState> pso(bool depth_enable, bool write, D3D12_COMPARISON_FUNC func, bool stencil = false,
                                    D3D12_STENCIL_OP pass_op = D3D12_STENCIL_OP_KEEP,
                                    D3D12_COMPARISON_FUNC stencil_func = D3D12_COMPARISON_FUNC_ALWAYS,
                                    bool color = true, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN)
    {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc =
            graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1,
                              color ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN);
        desc.DSVFormat = format == DXGI_FORMAT_UNKNOWN ? dsv_format : format;
        desc.DepthStencilState.DepthEnable = depth_enable;
        desc.DepthStencilState.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = func;
        desc.DepthStencilState.StencilEnable = stencil;
        desc.DepthStencilState.StencilReadMask = 0xff;
        desc.DepthStencilState.StencilWriteMask = 0xff;
        for (D3D12_DEPTH_STENCILOP_DESC *face : {&desc.DepthStencilState.FrontFace, &desc.DepthStencilState.BackFace}) {
            face->StencilFailOp = D3D12_STENCIL_OP_KEEP;
            face->StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
            face->StencilPassOp = pass_op;
            face->StencilFunc = stencil_func;
        }
        return gpu.graphics_pso(desc);
    }

    // Clears the targets, runs the draws and leaves the target as a render target again.
    void render(const std::vector<Draw> &draws, float depth_clear = 1.0f, UINT8 stencil_clear = 0, bool with_color = true)
    {
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->SetGraphicsRootSignature(signature.Get());
            const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(with_color ? 1 : 0, with_color ? &rtv : nullptr, FALSE, &dsv);
            const float clear[4] = {0, 0, 1, 1};
            if (with_color)
                list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth_clear, stencil_clear, 0, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (const Draw &d : draws) {
                list->SetPipelineState(d.pso);
                list->SetGraphicsRoot32BitConstants(0, 4, d.color, 0);
                list->OMSetStencilRef(d.stencil_ref);
                list->IASetVertexBuffers(0, 1, &d.triangle->view);
                list->DrawInstanced(3, 1, 0, 0);
            }
        });
    }

    Image color() { return gpu.read_texture(target.Get(), 0, 4); }
};

constexpr float kRed[4] = {1, 0, 0, 1};
constexpr float kGreen[4] = {0, 1, 0, 1};
constexpr Pixel kRedPixel = {255, 0, 0, 255}, kGreenPixel = {0, 255, 0, 255}, kBluePixel = {0, 0, 255, 255};

float read_float(const Image &image, UINT x, UINT y)
{
    float v;
    std::memcpy(&v, image.at(x, y), 4);
    return v;
}

} // namespace

int main()
{
    Gpu gpu;

    // Descriptor increments are per heap type.
    for (auto type : {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                      D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_TYPE_DSV})
        CHECK(gpu.increment(type) >= 8 && gpu.increment(type) <= 64);

    Fixture f(gpu, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT);
    const Triangle big_far = make_triangle(gpu, true, 0.8f), small_near = make_triangle(gpu, false, 0.2f);
    const Triangle big_near = make_triangle(gpu, true, 0.2f), small_far = make_triangle(gpu, false, 0.8f);
    const Triangle small_mid = make_triangle(gpu, false, 0.5f);

    ComPtr<ID3D12PipelineState> less = f.pso(true, true, D3D12_COMPARISON_FUNC_LESS);
    ComPtr<ID3D12PipelineState> less_no_write = f.pso(true, false, D3D12_COMPARISON_FUNC_LESS);
    ComPtr<ID3D12PipelineState> always = f.pso(true, true, D3D12_COMPARISON_FUNC_ALWAYS);
    ComPtr<ID3D12PipelineState> no_depth = f.pso(false, false, D3D12_COMPARISON_FUNC_LESS);

    // ---- Depth testing ---------------------------------------------------------------------------------------------
    {
        // The far triangle first, then a nearer one over part of it.
        f.render({{&big_far, {1, 0, 0, 1}, less.Get()}, {&small_near, {0, 1, 0, 1}, less.Get()}});
        Image image = f.color();
        expect_pixel("near triangle wins", image.pixel(32, 32), kGreenPixel);
        expect_pixel("far triangle elsewhere", image.pixel(2, 2), kRedPixel);

        // The nearer triangle first: the far one must not overwrite it.
        f.render({{&small_near, {0, 1, 0, 1}, less.Get()}, {&big_far, {1, 0, 0, 1}, less.Get()}});
        image = f.color();
        expect_pixel("far triangle fails the depth test", image.pixel(32, 32), kGreenPixel);
        expect_pixel("far triangle elsewhere (2)", image.pixel(2, 2), kRedPixel);

        // A near triangle over a small far one.
        f.render({{&small_far, {0, 1, 0, 1}, less.Get()}, {&big_near, {1, 0, 0, 1}, less.Get()}});
        image = f.color();
        expect_pixel("near covers far", image.pixel(32, 32), kRedPixel);

        // No depth test: the last draw wins.
        f.render({{&small_near, {0, 1, 0, 1}, always.Get()}, {&big_far, {1, 0, 0, 1}, always.Get()}});
        image = f.color();
        expect_pixel("ALWAYS, last wins", image.pixel(32, 32), kRedPixel);

        // Depth test disabled in the PSO: depth is neither tested nor written.
        f.render({{&small_near, {0, 1, 0, 1}, no_depth.Get()}, {&big_far, {1, 0, 0, 1}, no_depth.Get()}});
        image = f.color();
        expect_pixel("depth disabled", image.pixel(32, 32), kRedPixel);

        // Depth writes off: the first draw leaves the depth buffer alone, so the second passes.
        f.render({{&small_near, {1, 0, 0, 1}, less_no_write.Get()}, {&small_mid, {0, 1, 0, 1}, less.Get()}});
        image = f.color();
        expect_pixel("no depth write", image.pixel(32, 32), kGreenPixel);
    }

    // ---- The depth plane copied to a buffer (and a clear value) ---------------------------------------------------------
    {
        f.render({{&small_near, {1, 0, 0, 1}, less.Get()}}, 0.75f);
        Image depth = gpu.read_texture(f.depth.Get(), 0, 4);
        CHECK(std::fabs(read_float(depth, 2, 2) - 0.75f) < 1e-6f);
        CHECK(std::fabs(read_float(depth, 32, 32) - 0.2f) < 1e-5f);
    }

    // ---- A depth texture read by a shader through an SRV of the typeless resource ---------------------------------------
    {
        f.render({{&small_near, {1, 0, 0, 1}, less.Get()}}, 0.5f);
        Probe probe(gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT);
        srv.Texture2D.MipLevels = 1;
        probe.set_srv(0, f.depth.Get(), srv);
        const std::vector<Float4> got = probe.run(4, {{32, 32, 0, 0}, {2, 2, 0, 0}});
        CHECK(std::fabs(got[0].x - 0.2f) < 1e-5f);
        CHECK(std::fabs(got[1].x - 0.5f) < 1e-5f);
    }

    // ---- A depth-only pass: no render target, no fragment output -----------------------------------------------------------
    {
        ComPtr<ID3D12PipelineState> depth_only = f.pso(true, true, D3D12_COMPARISON_FUNC_LESS, false, D3D12_STENCIL_OP_KEEP,
                                                       D3D12_COMPARISON_FUNC_ALWAYS, false);
        f.render({{&small_near, {1, 0, 0, 1}, depth_only.Get()}}, 1.0f, 0, false);
        const Image depth = gpu.read_texture(f.depth.Get(), 0, 4);
        CHECK(std::fabs(read_float(depth, 32, 32) - 0.2f) < 1e-5f);
        CHECK(read_float(depth, 2, 2) == 1.0f);
    }

    // ---- Read-only depth view -----------------------------------------------------------------------------------------------
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        gpu.device->CreateDepthStencilView(f.depth.Get(), &dsv, gpu.cpu_handle(f.dsv_heap.Get(), 1));
        // Depth is read (tested) but the view cannot be written: the test passes where the nearer triangle is.
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = f.rtv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE rw = gpu.cpu_handle(f.dsv_heap.Get(), 0), ro = gpu.cpu_handle(f.dsv_heap.Get(), 1);
            list->SetGraphicsRootSignature(f.signature.Get());
            const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, &rw);
            const float clear[4] = {0, 0, 1, 1};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            list->ClearDepthStencilView(rw, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->SetPipelineState(less.Get());
            list->SetGraphicsRoot32BitConstants(0, 4, kGreen, 0);
            list->IASetVertexBuffers(0, 1, &small_near.view);
            list->DrawInstanced(3, 1, 0, 0);          // writes depth 0.2 in the middle
            list->OMSetRenderTargets(1, &rtv, FALSE, &ro);
            list->SetGraphicsRoot32BitConstants(0, 4, kRed, 0);
            list->IASetVertexBuffers(0, 1, &big_far.view);
            list->DrawInstanced(3, 1, 0, 0);          // tested against it, drawn only outside the middle
        });
        const Image image = f.color();
        expect_pixel("read-only depth view, middle", image.pixel(32, 32), kGreenPixel);
        expect_pixel("read-only depth view, outside", image.pixel(2, 2), kRedPixel);
    }

    // ---- Stencil ---------------------------------------------------------------------------------------------------------
    {
        Fixture s(gpu, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT);
        ComPtr<ID3D12PipelineState> write_one = s.pso(false, false, D3D12_COMPARISON_FUNC_ALWAYS, true, D3D12_STENCIL_OP_REPLACE);
        ComPtr<ID3D12PipelineState> test_equal = s.pso(false, false, D3D12_COMPARISON_FUNC_ALWAYS, true, D3D12_STENCIL_OP_KEEP,
                                                       D3D12_COMPARISON_FUNC_EQUAL);
        ComPtr<ID3D12PipelineState> increment = s.pso(false, false, D3D12_COMPARISON_FUNC_ALWAYS, true, D3D12_STENCIL_OP_INCR_SAT);
        const Triangle small = make_triangle(gpu, false, 0.5f), big = make_triangle(gpu, true, 0.5f);

        // The small triangle writes stencil 1; the big one draws only where it is 1.
        s.render({{&small, {0, 1, 0, 1}, write_one.Get(), 1}, {&big, {1, 0, 0, 1}, test_equal.Get(), 1}});
        Image image = s.color();
        expect_pixel("stencil test passes inside", image.pixel(32, 32), kRedPixel);
        expect_pixel("stencil test fails outside", image.pixel(2, 2), kBluePixel);

        // The planes of the depth-stencil texture (subresource 0 depth, 1 stencil), as D24S8 and the clear values.
        s.render({{&small, {0, 1, 0, 1}, increment.Get(), 0}, {&small, {0, 1, 0, 1}, increment.Get(), 0}}, 0.25f, 7);
        const Image stencil = gpu.read_texture(s.depth.Get(), 1, 1);
        CHECK_EQ(stencil.data[2 * stencil.width + 2], 7);
        CHECK_EQ(stencil.data[32 * stencil.width + 32], 9);
        const Image depth = gpu.read_texture(s.depth.Get(), 0, 4);
        CHECK(std::fabs(read_float(depth, 2, 2) - 0.25f) < 1e-6f);

        // Clearing one plane leaves the other alone.
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->ClearDepthStencilView(s.dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_STENCIL, 0.0f, 3, 0, nullptr);
        });
        const Image stencil2 = gpu.read_texture(s.depth.Get(), 1, 1);
        CHECK_EQ(stencil2.data[32 * stencil2.width + 32], 3);
        const Image depth2 = gpu.read_texture(s.depth.Get(), 0, 4);
        CHECK(std::fabs(read_float(depth2, 2, 2) - 0.25f) < 1e-6f);

        // Footprints of the planes.
        const D3D12_RESOURCE_DESC desc = s.depth->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2];
        UINT rows[2];
        UINT64 row_sizes[2], total;
        gpu.device->GetCopyableFootprints(&desc, 0, 2, 0, footprints, rows, row_sizes, &total);
        CHECK_EQ(row_sizes[0], kSize * 4u);
        CHECK_EQ(row_sizes[1], kSize * 1u);
        CHECK(footprints[0].Footprint.Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
        CHECK(footprints[1].Footprint.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT);
    }

    std::printf("p_depth: PASS\n");
    return 0;
}
