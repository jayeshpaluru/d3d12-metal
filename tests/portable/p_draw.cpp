// Draw variations: primitive topologies, per-instance data with step rates, StartInstanceLocation,
// BaseVertexLocation, adjacency topologies (skipped without a geometry shader) and bundles.
#include "color_ps.h"
#include "color_vs.h"
#include "instanced_ps.h"
#include "instanced_vs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 64;

struct InstanceData {
    float offset[2];
    uint8_t color[4];
};
static_assert(sizeof(InstanceData) == 12, "instance stride");

struct Fixture {
    Gpu &gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;

    explicit Fixture(Gpu &g) : gpu(g)
    {
        signature = gpu.root_signature();
        target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    ComPtr<ID3D12PipelineState> pso(D3D12_PRIMITIVE_TOPOLOGY_TYPE type, UINT instance_step_rate = 1)
    {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, instance_step_rate},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, instance_step_rate}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc =
            graphics_pso_desc(signature.Get(), T12_SHADER(g_instanced_vs), T12_SHADER(g_instanced_ps), layout, 3);
        desc.PrimitiveTopologyType = type;
        return gpu.graphics_pso(desc);
    }

    // Sets up the target (cleared to black) and the common state in `list`.
    void begin(ID3D12GraphicsCommandList *list, ID3D12PipelineState *state, D3D12_PRIMITIVE_TOPOLOGY topology)
    {
        list->SetGraphicsRootSignature(signature.Get());
        list->SetPipelineState(state);
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {0, 0, 0, 1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->IASetPrimitiveTopology(topology);
    }

    Image render(ID3D12PipelineState *state, D3D12_PRIMITIVE_TOPOLOGY topology,
                 const std::function<void(ID3D12GraphicsCommandList *)> &body)
    {
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            begin(list, state, topology);
            body(list);
        });
        return gpu.read_texture(target.Get(), 0, 4);
    }
};

// Pixel position of a point in normalised device coordinates.
std::pair<UINT, UINT> pixel_at(float x, float y)
{
    return {UINT((x + 1.0f) * 0.5f * kSize), UINT((1.0f - y) * 0.5f * kSize)};
}

constexpr Pixel kBlack = {0, 0, 0, 255}, kRed = {255, 0, 0, 255}, kGreen = {0, 255, 0, 255}, kBlue = {0, 0, 255, 255},
                kWhite = {255, 255, 255, 255};

// True when a pixel within `radius` of the position is `expected`.
bool near_colour(const Image &image, float x, float y, Pixel expected, int radius)
{
    const auto p = pixel_at(x, y);
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (near_pixel(image.pixel(UINT(int(p.first) + dx), UINT(int(p.second) + dy)), expected))
                return true;
        }
    }
    return false;
}

void expect_colour(const char *what, const Image &image, float x, float y, Pixel expected, int radius = 0)
{
    if (!near_colour(image, x, y, expected, radius)) {
        const auto p = pixel_at(x, y);
        const Pixel actual = image.pixel(p.first, p.second);
        std::fprintf(stderr, "%s: at (%u, %u) got (%u,%u,%u,%u)\n", what, p.first, p.second, actual.r, actual.g, actual.b, actual.a);
        std::abort();
    }
}

} // namespace

int main()
{
    Gpu gpu;
    Fixture f(gpu);

    // A square of half-size 0.1 as a triangle strip.
    const float square[8] = {-0.1f, -0.1f, -0.1f, 0.1f, 0.1f, -0.1f, 0.1f, 0.1f};
    ComPtr<ID3D12Resource> square_buffer = gpu.upload_buffer(square, sizeof(square));
    const D3D12_VERTEX_BUFFER_VIEW square_view = {square_buffer->GetGPUVirtualAddress(), sizeof(square), 8};

    const InstanceData instances[4] = {{{-0.5f, 0.5f}, {255, 0, 0, 255}},
                                       {{0.5f, 0.5f}, {0, 255, 0, 255}},
                                       {{0.0f, -0.5f}, {0, 0, 255, 255}},
                                       {{0.5f, -0.5f}, {255, 255, 255, 255}}};
    ComPtr<ID3D12Resource> instance_buffer = gpu.upload_buffer(instances, sizeof(instances));
    const D3D12_VERTEX_BUFFER_VIEW instance_view = {instance_buffer->GetGPUVirtualAddress(), sizeof(instances), 12};
    const D3D12_VERTEX_BUFFER_VIEW both[2] = {square_view, instance_view};

    // One white instance at the origin, for geometry that is placed by its own vertices.
    const InstanceData origin = {{0.0f, 0.0f}, {255, 255, 255, 255}};
    ComPtr<ID3D12Resource> origin_buffer = gpu.upload_buffer(&origin, sizeof(origin));
    const D3D12_VERTEX_BUFFER_VIEW origin_view = {origin_buffer->GetGPUVirtualAddress(), sizeof(origin), 12};

    auto triangles = f.pso(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);

    // ---- Triangle strip, three instances ----------------------------------------------------------------------------
    {
        const Image image = f.render(triangles.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, [&](ID3D12GraphicsCommandList *list) {
            list->IASetVertexBuffers(0, 2, both);
            // Games unbind the index buffer with a view of format UNKNOWN; a non-indexed draw is unaffected.
            const D3D12_INDEX_BUFFER_VIEW unbound = {0, 0, DXGI_FORMAT_UNKNOWN};
            list->IASetIndexBuffer(&unbound);
            list->DrawInstanced(4, 3, 0, 0);
        });
        expect_colour("strip instance 0", image, -0.5f, 0.5f, kRed);
        expect_colour("strip instance 1", image, 0.5f, 0.5f, kGreen);
        expect_colour("strip instance 2", image, 0.0f, -0.5f, kBlue);
        expect_colour("strip instance 3 not drawn", image, 0.5f, -0.5f, kBlack);
        expect_colour("strip centre", image, 0.0f, 0.0f, kBlack);
    }

    // ---- StartInstanceLocation: instance data starts at the location -------------------------------------------------
    {
        const Image image = f.render(triangles.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, [&](ID3D12GraphicsCommandList *list) {
            list->IASetVertexBuffers(0, 2, both);
            list->DrawInstanced(4, 2, 0, 1);
        });
        expect_colour("start instance: instance 0 skipped", image, -0.5f, 0.5f, kBlack);
        expect_colour("start instance: instance 1", image, 0.5f, 0.5f, kGreen);
        expect_colour("start instance: instance 2", image, 0.0f, -0.5f, kBlue);
    }

    // ---- Instance step rate 2: two consecutive instances share one element of instance data --------------------
    {
        auto stepped = f.pso(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 2);
        const Image image = f.render(stepped.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, [&](ID3D12GraphicsCommandList *list) {
            list->IASetVertexBuffers(0, 2, both);
            list->DrawInstanced(4, 4, 0, 0);  // instances 0,1 read element 0; 2,3 read element 1
        });
        expect_colour("step rate: element 0", image, -0.5f, 0.5f, kRed);
        expect_colour("step rate: element 1", image, 0.5f, 0.5f, kGreen);
        expect_colour("step rate: element 2 unused", image, 0.0f, -0.5f, kBlack);
    }

    // ---- Indexed draw with a base vertex and a start index ---------------------------------------------------------
    {
        // Two squares' worth of vertices; the second is shifted down by 0.5. The indices select a strip of four.
        float vertices[16];
        for (int i = 0; i < 4; ++i) {
            vertices[i * 2] = square[i * 2];
            vertices[i * 2 + 1] = square[i * 2 + 1];
            vertices[8 + i * 2] = square[i * 2];
            vertices[8 + i * 2 + 1] = square[i * 2 + 1] - 0.5f;
        }
        const uint16_t indices[8] = {0, 0, 0, 0, 0, 1, 2, 3};  // the strip starts at index 4
        ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(vertices, sizeof(vertices));
        ComPtr<ID3D12Resource> index_buffer = gpu.upload_buffer(indices, sizeof(indices));
        const D3D12_VERTEX_BUFFER_VIEW views[2] = {{vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 8}, origin_view};
        const D3D12_INDEX_BUFFER_VIEW ibv = {index_buffer->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
        const Image image = f.render(triangles.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, [&](ID3D12GraphicsCommandList *list) {
            list->IASetVertexBuffers(0, 2, views);
            list->IASetIndexBuffer(&ibv);
            list->DrawIndexedInstanced(4, 1, 4, 4, 0);  // base vertex 4: the shifted square
        });
        expect_colour("base vertex: shifted square", image, 0.0f, -0.5f, kWhite);
        expect_colour("base vertex: unshifted position empty", image, 0.0f, 0.0f, kBlack);
    }

    // ---- Line list, line strip, point list ---------------------------------------------------------------------
    {
        // Segments along y = 0 (x from -0.5 to 0.5) and x = 0 (y from -0.8 to 0.8), then a third vertex for strips.
        const float points[12] = {-0.5f, 0.0f, 0.5f, 0.0f, 0.0f, -0.8f, 0.0f, 0.8f, 0.5f, 0.8f, -0.5f, -0.8f};
        ComPtr<ID3D12Resource> point_buffer = gpu.upload_buffer(points, sizeof(points));
        const D3D12_VERTEX_BUFFER_VIEW views[2] = {{point_buffer->GetGPUVirtualAddress(), sizeof(points), 8}, origin_view};

        auto lines = f.pso(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
        const Image line_list = f.render(lines.Get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST, [&](ID3D12GraphicsCommandList *list) {
            list->IASetVertexBuffers(0, 2, views);
            list->DrawInstanced(4, 1, 0, 0);  // two segments
        });
        expect_colour("line list: horizontal", line_list, 0.25f, 0.0f, kWhite, 1);
        expect_colour("line list: vertical", line_list, 0.0f, 0.5f, kWhite, 1);
        expect_colour("line list: nothing off the lines", line_list, 0.5f, 0.5f, kBlack);

        const Image line_strip = f.render(lines.Get(), D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, [&](ID3D12GraphicsCommandList *list) {
            list->IASetVertexBuffers(0, 2, views);
            list->DrawInstanced(3, 1, 0, 0);  // (-0.5,0) -> (0.5,0) -> (0,-0.8)
        });
        expect_colour("line strip: first segment", line_strip, -0.25f, 0.0f, kWhite, 1);
        expect_colour("line strip: second segment", line_strip, 0.25f, -0.4f, kWhite, 1);
        expect_colour("line strip: no segment from the last to the first vertex", line_strip, -0.25f, -0.4f, kBlack);

        auto dots = f.pso(D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
        const Image point_list = f.render(dots.Get(), D3D_PRIMITIVE_TOPOLOGY_POINTLIST, [&](ID3D12GraphicsCommandList *list) {
            list->IASetVertexBuffers(0, 2, views);
            list->DrawInstanced(2, 1, 0, 0);  // (-0.5,0) and (0.5,0)
        });
        expect_colour("point list: first", point_list, -0.5f, 0.0f, kWhite, 1);
        expect_colour("point list: second", point_list, 0.5f, 0.0f, kWhite, 1);
        expect_colour("point list: nothing between", point_list, 0.0f, 0.0f, kBlack);
    }

    // ---- An adjacency topology needs a geometry shader: a plain pipeline draws nothing with it -------------------
    {
        const Image image = f.render(triangles.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, [&](ID3D12GraphicsCommandList *list) {
            list->IASetVertexBuffers(0, 2, both);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ);
            list->DrawInstanced(4, 1, 0, 0);
        });
        expect_colour("adjacency topology skipped", image, -0.5f, 0.5f, kBlack);
    }

    // ---- Bundles: recorded once, executed twice with different targets state ------------------------------------
    {
        ComPtr<ID3D12GraphicsCommandList> bundle = gpu.list(D3D12_COMMAND_LIST_TYPE_BUNDLE);
        bundle->SetGraphicsRootSignature(f.signature.Get());
        bundle->SetPipelineState(triangles.Get());
        bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        bundle->IASetVertexBuffers(0, 2, both);
        bundle->DrawInstanced(4, 2, 0, 0);
        CHECK_HR(bundle->Close());

        for (int round = 0; round < 2; ++round) {
            const Image image = f.render(triangles.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, [&](ID3D12GraphicsCommandList *list) {
                list->ExecuteBundle(bundle.Get());
                // The state the bundle set is the list's afterwards: this draws the third instance.
                list->DrawInstanced(4, 1, 0, 2);
            });
            expect_colour("bundle instance 0", image, -0.5f, 0.5f, kRed);
            expect_colour("bundle instance 1", image, 0.5f, 0.5f, kGreen);
            expect_colour("draw after bundle", image, 0.0f, -0.5f, kBlue);
            expect_colour("bundle draws two instances only", image, 0.5f, -0.5f, kBlack);
        }
    }

    // ---- Root arguments a bundle sets after its last draw reach the next draw of the list ------------------------------
    {
        const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
        ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&constants, 1);
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        ComPtr<ID3D12PipelineState> pso =
            gpu.graphics_pso(graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1));
        // Triangle 0 in the upper left quadrant, triangle 1 in the lower right.
        const float vertices[18] = {-0.8f, 0.2f, 0.5f, -0.2f, 0.2f, 0.5f, -0.5f, 0.8f, 0.5f,
                                    0.2f, -0.8f, 0.5f, 0.8f, -0.8f, 0.5f, 0.5f, -0.2f, 0.5f};
        ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(vertices, sizeof(vertices));
        const D3D12_VERTEX_BUFFER_VIEW view = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 12};
        const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};

        ComPtr<ID3D12GraphicsCommandList> bundle = gpu.list(D3D12_COMMAND_LIST_TYPE_BUNDLE);
        bundle->SetGraphicsRootSignature(signature.Get());
        bundle->SetPipelineState(pso.Get());
        bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bundle->IASetVertexBuffers(0, 1, &view);
        bundle->SetGraphicsRoot32BitConstants(0, 4, red, 0);
        bundle->DrawInstanced(3, 1, 0, 0);
        bundle->SetGraphicsRoot32BitConstants(0, 4, green, 0);  // no draw after this in the bundle
        CHECK_HR(bundle->Close());

        const Image image = f.render(pso.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, [&](ID3D12GraphicsCommandList *list) {
            list->SetGraphicsRootSignature(signature.Get());
            list->ExecuteBundle(bundle.Get());
            list->DrawInstanced(3, 1, 3, 0);
        });
        expect_colour("bundle draw uses its own arguments", image, -0.5f, 0.4f, kRed);
        expect_colour("draw after the bundle uses the arguments it left", image, 0.5f, -0.4f, kGreen);
    }

    std::printf("p_draw: PASS\n");
    return 0;
}
