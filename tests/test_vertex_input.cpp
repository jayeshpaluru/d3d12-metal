// SPDX-License-Identifier: LGPL-2.1-or-later
// Vertex input through the converter's stage-in function: two vertex buffer
// slots, three attributes of different formats. Slot 0 interleaves a float3
// position and a float2 offset (stride 20); slot 1 holds R8G8B8A8_UNORM colours
// (stride 4). The interpolated colours are checked pixel by pixel.
#include <cmath>

#include "render_context.h"
#include "vertex_input_ps.h"
#include "vertex_input_vs.h"

int main()
{
    RenderContext ctx;

    ComPtr<ID3D12RootSignature> signature = ctx.create_root_signature();

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    ComPtr<ID3D12PipelineState> pso = ctx.create_pso(
        signature.Get(), {g_vertex_input_vs, sizeof(g_vertex_input_vs)}, {g_vertex_input_ps, sizeof(g_vertex_input_ps)},
        layout, 3);

    // Clip-space triangle, shifted right by the per-vertex offset.
    constexpr float kShift = 0.25f;
    const float positions[3][2] = {{-0.5f, -0.5f}, {0.0f, 0.5f}, {0.5f, -0.5f}};
    float slot0[3][5];
    for (int i = 0; i < 3; ++i) {
        const float v[5] = {positions[i][0], positions[i][1], 0.0f, kShift, 0.0f};
        std::memcpy(slot0[i], v, sizeof(v));
    }
    const Pixel colors[3] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    ComPtr<ID3D12Resource> vb0 = ctx.create_upload_buffer(slot0, sizeof(slot0));
    ComPtr<ID3D12Resource> vb1 = ctx.create_upload_buffer(colors, sizeof(colors));
    const D3D12_VERTEX_BUFFER_VIEW views[2] = {
        {vb0->GetGPUVirtualAddress(), sizeof(slot0), sizeof(slot0[0])},
        {vb1->GetGPUVirtualAddress(), sizeof(colors), sizeof(Pixel)},
    };

    ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list(pso.Get());
    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.rtv();
    list->SetGraphicsRootSignature(signature.Get());
    ctx.set_viewport_and_scissor(list.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 2, views);
    list->DrawInstanced(3, 1, 0, 0);

    UINT row_pitch = 0;
    ComPtr<ID3D12Resource> readback = ctx.record_readback(list.Get(), &row_pitch);
    CHECK_HR(list->Close());
    ctx.execute_and_wait(list.Get());

    // Expected colour at a pixel centre: barycentric blend of the vertex colours.
    auto expected_at = [&](UINT px, UINT py) {
        double x = (px + 0.5) / kTargetSize * 2.0 - 1.0 - kShift;
        double y = 1.0 - (py + 0.5) / kTargetSize * 2.0;
        double ax = positions[0][0], ay = positions[0][1], bx = positions[1][0], by = positions[1][1],
               cx = positions[2][0], cy = positions[2][1];
        double det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        double w0 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / det;
        double w1 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / det;
        double w2 = 1.0 - w0 - w1;
        CHECK(w0 > 0 && w1 > 0 && w2 > 0);
        return Pixel{uint8_t(std::lround(w0 * 255)), uint8_t(std::lround(w1 * 255)), uint8_t(std::lround(w2 * 255)), 255};
    };
    auto check = [&](const char *what, UINT px, UINT py) {
        Pixel actual = read_pixel(readback.Get(), row_pitch, px, py), expected = expected_at(px, py);
        auto close = [](uint8_t a, uint8_t b) { return std::abs(int(a) - int(b)) <= 6; };
        if (!(close(actual.r, expected.r) && close(actual.g, expected.g) && close(actual.b, expected.b))) {
            std::fprintf(stderr, "%s: got (%u,%u,%u), expected (%u,%u,%u)\n", what, actual.r, actual.g, actual.b,
                         expected.r, expected.g, expected.b);
            std::exit(1);
        }
    };
    check("centre", 40, 36);
    check("near red", 28, 44);
    check("near green", 40, 20);
    check("near blue", 52, 44);
    // The offset moved the triangle: the unshifted centre is outside it.
    check_pixel("left of triangle", read_pixel(readback.Get(), row_pitch, 18, 44), {0, 0, 0, 255});
    check_pixel("corner", read_pixel(readback.Get(), row_pitch, 0, 0), {0, 0, 0, 255});
    return 0;
}
