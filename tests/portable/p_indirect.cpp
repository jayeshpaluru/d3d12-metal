// SPDX-License-Identifier: LGPL-2.1-or-later
// ExecuteIndirect: draws, indexed draws and dispatches read from an argument buffer, with and without a
// count buffer, and signatures whose commands also change root constants and vertex buffers.
#include "color_ps.h"
#include "color_vs.h"
#include "fill_cs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 64;

struct Fixture {
    Gpu &gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12Resource> vertices, indices;
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    D3D12_INDEX_BUFFER_VIEW ibv = {};

    explicit Fixture(Gpu &g) : gpu(g)
    {
        const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
        signature = gpu.root_signature(&constants, 1);
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        pso = gpu.graphics_pso(graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1));
        target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        // Four small triangles, one in each quadrant: 12 vertices (and as indices, in reverse order of triangles).
        std::vector<float> v;
        std::vector<uint16_t> ix;
        const float centres[4][2] = {{-0.5f, 0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};
        for (int q = 0; q < 4; ++q) {
            const float cx = centres[q][0], cy = centres[q][1];
            const float tri[9] = {cx - 0.2f, cy - 0.2f, 0.5f, cx + 0.2f, cy - 0.2f, 0.5f, cx, cy + 0.2f, 0.5f};
            v.insert(v.end(), tri, tri + 9);
        }
        for (int q = 0; q < 4; ++q) {
            for (int k = 0; k < 3; ++k)
                ix.push_back(static_cast<uint16_t>(q * 3 + k));
        }
        vertices = gpu.upload_buffer(v.data(), v.size() * 4);
        indices = gpu.upload_buffer(ix.data(), ix.size() * 2);
        vbv = {vertices->GetGPUVirtualAddress(), UINT(v.size() * 4), 12};
        ibv = {indices->GetGPUVirtualAddress(), UINT(ix.size() * 2), DXGI_FORMAT_R16_UINT};
    }

    ComPtr<ID3D12CommandSignature> command_signature(const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &args, UINT stride,
                                                     ID3D12RootSignature *root = nullptr)
    {
        D3D12_COMMAND_SIGNATURE_DESC desc = {};
        desc.ByteStride = stride;
        desc.NumArgumentDescs = static_cast<UINT>(args.size());
        desc.pArgumentDescs = args.data();
        ComPtr<ID3D12CommandSignature> signature;
        CHECK_HR(gpu.device->CreateCommandSignature(&desc, root, IID_PPV_ARGS(signature.GetAddressOf())));
        return signature;
    }

    // Records the common state, lets `body` draw, and reads the target back.
    Image render(const std::function<void(ID3D12GraphicsCommandList *)> &body)
    {
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
            list->IASetVertexBuffers(0, 1, &vbv);
            list->IASetIndexBuffer(&ibv);
            const float white[4] = {1, 1, 1, 1};
            list->SetGraphicsRoot32BitConstants(0, 4, white, 0);
            body(list);
        });
        return gpu.read_texture(target.Get(), 0, 4);
    }
};

D3D12_INDIRECT_ARGUMENT_DESC argument(D3D12_INDIRECT_ARGUMENT_TYPE type)
{
    D3D12_INDIRECT_ARGUMENT_DESC a = {};
    a.Type = type;
    return a;
}

constexpr Pixel kWhite = {255, 255, 255, 255}, kBlack = {0, 0, 0, 255};
const std::pair<UINT, UINT> kQuadrantCentre[4] = {{16, 16}, {48, 16}, {16, 48}, {48, 48}};

} // namespace

int main()
{
    Gpu gpu;
    Fixture f(gpu);

    // ---- DRAW with a count buffer: only `count` of the commands run ------------------------------------------------
    {
        auto sig = f.command_signature({argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 16);
        std::vector<D3D12_DRAW_ARGUMENTS> commands;
        for (UINT q = 0; q < 4; ++q)
            commands.push_back({3, 1, q * 3, 0});
        ComPtr<ID3D12Resource> arguments = gpu.upload_buffer(commands.data(), commands.size() * 16);
        for (UINT count : {0u, 1u, 3u, 4u, 9u}) {
            ComPtr<ID3D12Resource> count_buffer = gpu.upload_buffer(&count, 4);
            const Image image = f.render([&](ID3D12GraphicsCommandList *list) {
                list->ExecuteIndirect(sig.Get(), 4, arguments.Get(), 0, count_buffer.Get(), 0);
            });
            for (UINT q = 0; q < 4; ++q)
                expect_pixel("draw with count", image.pixel(kQuadrantCentre[q].first, kQuadrantCentre[q].second),
                             q < std::min(count, 4u) ? kWhite : kBlack);
        }
    }

    // ---- A count outside its buffer or unaligned skips the call; a max count beyond the arguments is clamped ------------
    {
        auto sig = f.command_signature({argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 16);
        std::vector<D3D12_DRAW_ARGUMENTS> commands;
        for (UINT q = 0; q < 4; ++q)
            commands.push_back({3, 1, q * 3, 0});
        ComPtr<ID3D12Resource> arguments = gpu.upload_buffer(commands.data(), commands.size() * 16);
        const UINT counts[2] = {4, 4};
        ComPtr<ID3D12Resource> count_buffer = gpu.upload_buffer(counts, sizeof(counts));
        for (UINT64 bad_offset : {UINT64(2), UINT64(8), UINT64(1) << 40}) {
            const Image image = f.render([&](ID3D12GraphicsCommandList *list) {
                list->ExecuteIndirect(sig.Get(), 4, arguments.Get(), 0, count_buffer.Get(), bad_offset);
            });
            for (UINT q = 0; q < 4; ++q)
                expect_pixel("count outside its buffer", image.pixel(kQuadrantCentre[q].first, kQuadrantCentre[q].second), kBlack);
        }
        const Image good = f.render([&](ID3D12GraphicsCommandList *list) {
            list->ExecuteIndirect(sig.Get(), 4, arguments.Get(), 0, count_buffer.Get(), 4);
        });
        expect_pixel("count at a valid offset", good.pixel(48, 48), kWhite);

        // No count buffer and a max count of 1000: only the four commands in the buffer exist.
        const Image clamped = f.render([&](ID3D12GraphicsCommandList *list) {
            list->ExecuteIndirect(sig.Get(), 1000, arguments.Get(), 0, nullptr, 0);
        });
        for (UINT q = 0; q < 4; ++q)
            expect_pixel("max count clamped to the buffer", clamped.pixel(kQuadrantCentre[q].first, kQuadrantCentre[q].second), kWhite);
        // An argument offset past the buffer draws nothing.
        const Image past = f.render([&](ID3D12GraphicsCommandList *list) {
            list->ExecuteIndirect(sig.Get(), 4, arguments.Get(), 4096, nullptr, 0);
        });
        expect_pixel("arguments past the buffer", past.pixel(16, 16), kBlack);
    }

    // ---- DRAW without a count, a stride larger than the command, and a nonzero buffer offset ----------------------------
    {
        auto sig = f.command_signature({argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 32);
        std::vector<uint32_t> storage(8 + 4 * 8, 0);
        for (UINT q = 0; q < 4; ++q) {
            storage[8 + q * 8 + 0] = 3;
            storage[8 + q * 8 + 1] = 1;
            storage[8 + q * 8 + 2] = q * 3;
        }
        ComPtr<ID3D12Resource> arguments = gpu.upload_buffer(storage.data(), storage.size() * 4);
        const Image image = f.render([&](ID3D12GraphicsCommandList *list) {
            list->ExecuteIndirect(sig.Get(), 3, arguments.Get(), 32, nullptr, 0);  // three commands
        });
        for (UINT q = 0; q < 4; ++q)
            expect_pixel("draw without count", image.pixel(kQuadrantCentre[q].first, kQuadrantCentre[q].second), q < 3 ? kWhite : kBlack);
    }

    // ---- DRAW_INDEXED ----------------------------------------------------------------------------------------------------------
    {
        auto sig = f.command_signature({argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)}, 20);
        std::vector<D3D12_DRAW_INDEXED_ARGUMENTS> commands;
        for (UINT q = 0; q < 4; ++q)
            commands.push_back({3, 1, (3 - q) * 3 % 12, 0, 0});  // the triangles in another order
        ComPtr<ID3D12Resource> arguments = gpu.upload_buffer(commands.data(), commands.size() * 20);
        const UINT count = 2;
        ComPtr<ID3D12Resource> count_buffer = gpu.upload_buffer(&count, 4);
        const Image image = f.render([&](ID3D12GraphicsCommandList *list) {
            list->ExecuteIndirect(sig.Get(), 4, arguments.Get(), 0, count_buffer.Get(), 0);
        });
        // Commands 0 and 1 draw the triangles of quadrants 3 and 2.
        const bool drawn[4] = {false, false, true, true};
        for (UINT q = 0; q < 4; ++q)
            expect_pixel("indexed draw", image.pixel(kQuadrantCentre[q].first, kQuadrantCentre[q].second), drawn[q] ? kWhite : kBlack);
    }

    // ---- Root constants per command, then a draw ------------------------------------------------------------------------
    {
        D3D12_INDIRECT_ARGUMENT_DESC constants = argument(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT);
        constants.Constant.RootParameterIndex = 0;
        constants.Constant.DestOffsetIn32BitValues = 0;
        constants.Constant.Num32BitValuesToSet = 4;
        auto sig = f.command_signature({constants, argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 48, f.signature.Get());
        struct Command {
            float color[4];
            D3D12_DRAW_ARGUMENTS draw;
            uint32_t pad[4];
        };
        static_assert(sizeof(Command) == 48, "stride");
        const float colours[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
        std::vector<Command> commands(4);
        for (UINT q = 0; q < 4; ++q) {
            std::memcpy(commands[q].color, colours[q], 16);
            commands[q].draw = {3, 1, q * 3, 0};
            std::memset(commands[q].pad, 0, 16);
        }
        ComPtr<ID3D12Resource> arguments = gpu.upload_buffer(commands.data(), commands.size() * sizeof(Command));
        const UINT count = 4;
        ComPtr<ID3D12Resource> count_buffer = gpu.upload_buffer(&count, 4);
        const Image image = f.render([&](ID3D12GraphicsCommandList *list) {
            list->ExecuteIndirect(sig.Get(), 8, arguments.Get(), 0, count_buffer.Get(), 0);  // max 8, count 4
        });
        const Pixel expected[4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
        for (UINT q = 0; q < 4; ++q)
            expect_pixel("per-command constants", image.pixel(kQuadrantCentre[q].first, kQuadrantCentre[q].second), expected[q]);

        // The application's own constants are in force again afterwards.
        const Image after = f.render([&](ID3D12GraphicsCommandList *list) {
            list->ExecuteIndirect(sig.Get(), 4, arguments.Get(), 0, nullptr, 0);
            const float cyan[4] = {0, 1, 1, 1};
            list->SetGraphicsRoot32BitConstants(0, 4, cyan, 0);
            list->DrawInstanced(3, 1, 0, 0);
        });
        expect_pixel("indirect draw before a direct one", after.pixel(16, 16), {0, 255, 255, 255});
    }

    // ---- A vertex buffer per command ------------------------------------------------------------------------------------------
    {
        D3D12_INDIRECT_ARGUMENT_DESC vertex_buffer = argument(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW);
        vertex_buffer.VertexBuffer.Slot = 0;
        auto sig = f.command_signature({vertex_buffer, argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)}, 32);
        struct Command {
            D3D12_VERTEX_BUFFER_VIEW view;
            D3D12_DRAW_ARGUMENTS draw;
        };
        static_assert(sizeof(Command) == 32, "stride");
        // The same vertex memory seen from two different starting points: quadrant 3's triangle, then quadrant 1's.
        const UINT64 base = f.vertices->GetGPUVirtualAddress();
        Command commands[2] = {{{base + 9 * 12, 36, 12}, {3, 1, 0, 0}}, {{base + 3 * 12, 36, 12}, {3, 1, 0, 0}}};
        ComPtr<ID3D12Resource> arguments = gpu.upload_buffer(commands, sizeof(commands));
        const Image image = f.render([&](ID3D12GraphicsCommandList *list) {
            list->ExecuteIndirect(sig.Get(), 2, arguments.Get(), 0, nullptr, 0);
        });
        const bool drawn[4] = {false, true, false, true};
        for (UINT q = 0; q < 4; ++q)
            expect_pixel("vertex buffer per command", image.pixel(kQuadrantCentre[q].first, kQuadrantCentre[q].second), drawn[q] ? kWhite : kBlack);
    }

    // ---- Unsupported arguments are refused at creation ---------------------------------------------------------------------------
    {
        D3D12_COMMAND_SIGNATURE_DESC desc = {};
        D3D12_INDIRECT_ARGUMENT_DESC args[2] = {argument(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW),
                                                argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)};
        desc.ByteStride = 40;
        desc.NumArgumentDescs = 2;
        desc.pArgumentDescs = args;
        ComPtr<ID3D12CommandSignature> refused;
        CHECK(gpu.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(refused.GetAddressOf())) == E_NOTIMPL);
        // The action must be last.
        D3D12_INDIRECT_ARGUMENT_DESC bad[2] = {argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW), argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)};
        desc.ByteStride = 32;
        desc.pArgumentDescs = bad;
        CHECK(gpu.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(refused.GetAddressOf())) == E_INVALIDARG);
    }

    // ---- DISPATCH, with a count, and per-command root constants ---------------------------------------------------------------
    {
        const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0);
        const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), descriptor_table(&range, 1)};
        ComPtr<ID3D12RootSignature> compute_signature = gpu.root_signature(parameters, 2, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3D12PipelineState> compute_pso = gpu.compute_pso(compute_signature.Get(), T12_SHADER(g_fill_cs));
        ComPtr<ID3D12DescriptorHeap> heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);
        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, uav));
        ComPtr<ID3D12Resource> structured = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 1024, uav);
        ComPtr<ID3D12Resource> raw = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 1024, uav);
        for (UINT slot = 0; slot < 6; ++slot) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC null_desc = {};
            null_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            null_desc.Format = DXGI_FORMAT_R32_UINT;
            gpu.device->CreateUnorderedAccessView(nullptr, nullptr, &null_desc, gpu.cpu_handle(heap.Get(), slot));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 0));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 256;
        desc.Buffer.StructureByteStride = 4;
        gpu.device->CreateUnorderedAccessView(structured.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 1));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer.NumElements = 256;
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        gpu.device->CreateUnorderedAccessView(raw.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 2));

        D3D12_INDIRECT_ARGUMENT_DESC constants = argument(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT);
        constants.Constant.RootParameterIndex = 0;
        constants.Constant.Num32BitValuesToSet = 4;
        auto sig = f.command_signature({constants, argument(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)}, 32, compute_signature.Get());
        struct Command {
            uint32_t constants[4];
            D3D12_DISPATCH_ARGUMENTS dispatch;
            uint32_t pad;
        };
        static_assert(sizeof(Command) == 32, "stride");
        Command commands[2] = {{{16, 16, 4, 9}, {2, 2, 1}, 0},   // mode 4: raw buffer, value 9
                               {{16, 16, 0, 0}, {2, 2, 1}, 0}};  // mode 0: pattern into the texture and structured buffer
        ComPtr<ID3D12Resource> arguments = gpu.upload_buffer(commands, sizeof(commands));
        for (UINT count : {1u, 2u}) {
            // Reset both outputs.
            std::vector<uint32_t> zeros(256, 0);
            ComPtr<ID3D12Resource> zero_buffer = gpu.upload_buffer(zeros.data(), 1024);
            ComPtr<ID3D12Resource> count_buffer = gpu.upload_buffer(&count, 4);
            gpu.run([&](ID3D12GraphicsCommandList *list) {
                list->CopyBufferRegion(raw.Get(), 0, zero_buffer.Get(), 0, 1024);
                list->CopyBufferRegion(structured.Get(), 0, zero_buffer.Get(), 0, 1024);
                ID3D12DescriptorHeap *heaps[] = {heap.Get()};
                list->SetDescriptorHeaps(1, heaps);
                list->SetComputeRootSignature(compute_signature.Get());
                list->SetPipelineState(compute_pso.Get());
                list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), 0));
                const D3D12_RESOURCE_BARRIER barrier = uav_barrier(nullptr);
                list->ResourceBarrier(1, &barrier);
                list->ExecuteIndirect(sig.Get(), 2, arguments.Get(), 0, count_buffer.Get(), 0);
            });
            std::vector<uint8_t> bytes = gpu.read_buffer(raw.Get(), 1024);
            uint32_t words[256];
            std::memcpy(words, bytes.data(), sizeof(words));
            for (UINT i = 0; i < 256; ++i)
                CHECK_EQ(words[i], i * 3 + 9);
            bytes = gpu.read_buffer(structured.Get(), 1024);
            std::memcpy(words, bytes.data(), sizeof(words));
            CHECK_EQ(words[17], count == 2 ? (0xA5000000u | (1u << 8) | 1u) : 0u);
        }
    }

    std::printf("p_indirect: PASS\n");
    return 0;
}
