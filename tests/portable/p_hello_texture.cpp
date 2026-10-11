// SPDX-License-Identifier: LGPL-2.1-or-later
// D3D12HelloTexture and D3D12HelloConstBuffers (Microsoft's samples) rendered offscreen: the same root
// signatures (a descriptor table with a static sampler; a descriptor table with a constant buffer),
// resources, uploads and draws, with the output read back and compared to what the samples draw.
#include <cmath>

#include "const_buffers_ps.h"
#include "const_buffers_vs.h"
#include "hello_texture_ps.h"
#include "hello_texture_vs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 128;

struct Target {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;

    explicit Target(Gpu &gpu)
    {
        texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        gpu.device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
    }

    void set_viewport(ID3D12GraphicsCommandList *list) const
    {
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
    }
};

// ---- HelloTexture --------------------------------------------------------------------------------------

struct TextureVertex {
    float position[3];
    float uv[2];
};

constexpr UINT kTextureWidth = 256, kTextureHeight = 256, kTexturePixelSize = 4;

// GenerateTextureData of the sample.
std::vector<uint8_t> checkerboard()
{
    const UINT row_pitch = kTextureWidth * kTexturePixelSize;
    const UINT cell_pitch = row_pitch >> 3;
    const UINT cell_height = kTextureWidth >> 3;
    const UINT size = row_pitch * kTextureHeight;
    std::vector<uint8_t> data(size);
    for (UINT n = 0; n < size; n += kTexturePixelSize) {
        const UINT x = n % row_pitch, y = n / row_pitch;
        const UINT i = x / cell_pitch, j = y / cell_height;
        const uint8_t value = (i % 2 == j % 2) ? 0x00 : 0xff;
        data[n] = data[n + 1] = data[n + 2] = value;
        data[n + 3] = 0xff;
    }
    return data;
}

void hello_texture(Gpu &gpu)
{
    // Root signature: an SRV table for the pixel shader and a static point/border sampler.
    const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    const D3D12_ROOT_PARAMETER1 table = descriptor_table(&range, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    const D3D12_STATIC_SAMPLER_DESC sampler =
        static_sampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&table, 1, &sampler, 1);

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc =
        graphics_pso_desc(signature.Get(), T12_SHADER(g_hello_texture_vs), T12_SHADER(g_hello_texture_ps), layout, 2);
    ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(pso_desc);

    // The sample's triangle, scaled up to fill the target.
    const TextureVertex vertices[] = {
        {{0.0f, 0.9f, 0.0f}, {0.5f, 0.0f}},
        {{0.9f, -0.9f, 0.0f}, {1.0f, 1.0f}},
        {{-0.9f, -0.9f, 0.0f}, {0.0f, 1.0f}},
    };
    ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(vertices, sizeof(vertices));
    const D3D12_VERTEX_BUFFER_VIEW vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), sizeof(TextureVertex)};

    // The texture: created in COPY_DEST, filled through an upload buffer, then made readable by pixel shaders.
    ComPtr<ID3D12Resource> texture =
        gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kTextureWidth, kTextureHeight), D3D12_RESOURCE_STATE_COPY_DEST);
    const std::vector<uint8_t> pixels = checkerboard();
    gpu.upload_texture(texture.Get(), 0, pixels.data(), kTexturePixelSize);

    ComPtr<ID3D12DescriptorHeap> srv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    gpu.device->CreateShaderResourceView(texture.Get(), &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());

    Target target(gpu);
    ComPtr<ID3D12GraphicsCommandList> list = gpu.list(D3D12_COMMAND_LIST_TYPE_DIRECT, pso.Get());
    const D3D12_RESOURCE_BARRIER to_shader = transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_shader);
    list->SetGraphicsRootSignature(signature.Get());
    ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
    target.set_viewport(list.Get());
    list->OMSetRenderTargets(1, &target.rtv, FALSE, nullptr);
    const float clear[4] = {0.0f, 0.2f, 0.4f, 1.0f};
    list->ClearRenderTargetView(target.rtv, clear, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &vbv);
    list->DrawInstanced(3, 1, 0, 0);
    gpu.run(list.Get());

    const Image image = gpu.read_texture(target.texture.Get(), 0, 4);
    if (getenv("DUMP_ASCII")) {
        for (UINT py = 0; py < kSize; py += 4) {
            for (UINT px = 0; px < kSize; px += 2) {
                const Pixel p = image.pixel(px, py);
                std::fputc(p.r == 0 && p.g == 0 && p.b == 0 ? '#' : (p.r > 200 ? 'o' : '.'), stderr);
            }
            std::fputc('\n', stderr);
        }
    }
    int checked = 0, white = 0, black = 0;
    for (UINT py = 0; py < kSize; py += 4) {
        for (UINT px = 0; px < kSize; px += 4) {
            const float x = (px + 0.5f) / kSize * 2 - 1, y = 1 - (py + 0.5f) / kSize * 2;
            const float v = (0.9f - y) / 1.8f;
            const float u = 0.5f + x / 1.8f;  // linear in x: the triangle has no perspective
            // Inside the triangle |x| <= 0.9 v; keep clear of the edges.
            const bool inside = v > 0.05f && v < 0.98f && std::fabs(x) < 0.9f * v - 0.04f;
            const float fu = u * 8 - std::floor(u * 8), fv = v * 8 - std::floor(v * 8);
            const bool near_edge = fu < 0.15f || fu > 0.85f || fv < 0.15f || fv > 0.85f;
            const Pixel actual = image.pixel(px, py);
            if (!inside) {
                if (v < -0.05f || v > 1.05f || std::fabs(x) > 0.9f * v + 0.04f)
                    expect_pixel("outside the triangle", actual, {0, 51, 102, 255});
                continue;
            }
            if (near_edge)
                continue;
            const int cell_u = int(u * 8), cell_v = int(v * 8);
            const bool is_black = cell_u % 2 == cell_v % 2;
            expect_pixel("textured triangle", actual, is_black ? Pixel{0, 0, 0, 255} : Pixel{255, 255, 255, 255});
            ++checked;
            (is_black ? black : white)++;
        }
    }
    CHECK(checked > 100 && white > 20 && black > 20);
}

// ---- HelloConstBuffers ------------------------------------------------------------------------------------

struct ColorVertex {
    float position[3];
    float color[4];
};

void hello_const_buffers(Gpu &gpu)
{
    // Root signature: a CBV table for the vertex shader.
    const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    const D3D12_ROOT_PARAMETER1 table = descriptor_table(&range, 1, D3D12_SHADER_VISIBILITY_VERTEX);
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&table, 1);

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(
        graphics_pso_desc(signature.Get(), T12_SHADER(g_const_buffers_vs), T12_SHADER(g_const_buffers_ps), layout, 2));

    // The sample's triangle (aspect ratio 1).
    const ColorVertex vertices[] = {
        {{0.0f, 0.25f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.25f, -0.25f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{-0.25f, -0.25f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
    };
    ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(vertices, sizeof(vertices));
    const D3D12_VERTEX_BUFFER_VIEW vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), sizeof(ColorVertex)};

    // The scene constant buffer stays mapped; each frame writes a new offset into it.
    struct SceneConstantBuffer {
        float offset[4];
        float padding[60];
    };
    static_assert(sizeof(SceneConstantBuffer) % 256 == 0, "constant buffers are 256-byte aligned");
    ComPtr<ID3D12Resource> constant_buffer = gpu.buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(SceneConstantBuffer));
    ComPtr<ID3D12DescriptorHeap> cbv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {constant_buffer->GetGPUVirtualAddress(), sizeof(SceneConstantBuffer)};
    gpu.device->CreateConstantBufferView(&cbv, cbv_heap->GetCPUDescriptorHandleForHeapStart());
    SceneConstantBuffer* data = nullptr;
    CHECK_HR(constant_buffer->Map(0, nullptr, reinterpret_cast<void **>(&data)));
    std::memset(data, 0, sizeof(*data));

    Target target(gpu);
    float offset_x = 0.0f;
    for (int frame = 0; frame < 4; ++frame) {
        offset_x = -0.5f + 0.4f * frame;
        data->offset[0] = offset_x;  // the sample adds a small step each frame: the buffer is rewritten in place
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list(D3D12_COMMAND_LIST_TYPE_DIRECT, pso.Get());
        list->SetGraphicsRootSignature(signature.Get());
        ID3D12DescriptorHeap *heaps[] = {cbv_heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(0, cbv_heap->GetGPUDescriptorHandleForHeapStart());
        target.set_viewport(list.Get());
        list->OMSetRenderTargets(1, &target.rtv, FALSE, nullptr);
        const float clear[4] = {0.0f, 0.2f, 0.4f, 1.0f};
        list->ClearRenderTargetView(target.rtv, clear, 0, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->DrawInstanced(3, 1, 0, 0);
        gpu.run(list.Get());

        const Image image = gpu.read_texture(target.texture.Get(), 0, 4);
        // The centroid of the triangle (offset, -0.083): the three vertex colours average out.
        const UINT cx = UINT((offset_x + 1) / 2 * kSize), cy = UINT((1 + 0.0833f) / 2 * kSize);
        expect_pixel("triangle centroid", image.pixel(cx, cy), {85, 85, 85, 255}, 6);
        // Near each vertex the colour is that vertex's.
        const UINT top_x = UINT((offset_x + 1) / 2 * kSize);
        expect_pixel("red apex", image.pixel(top_x, UINT((1 - 0.2f) / 2 * kSize)), {240, 14, 14, 255}, 30);
        // Where the triangle was in the first frame there is nothing any more.
        if (frame > 0)
            expect_pixel("old position is clear", image.pixel(UINT(0.25f * kSize), UINT(0.5f * kSize + 4)), {0, 51, 102, 255});
        // The centre column of the target is the clear colour unless the triangle sits there.
        expect_pixel("far corner", image.pixel(kSize - 2, 2), {0, 51, 102, 255});
    }
    constant_buffer->Unmap(0, nullptr);
}

} // namespace

int main()
{
    Gpu gpu;
    hello_texture(gpu);
    hello_const_buffers(gpu);
    std::printf("p_hello_texture: PASS\n");
    return 0;
}
