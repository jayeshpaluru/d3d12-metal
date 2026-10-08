// Ports of Microsoft's D3D12HelloTexture and D3D12HelloConstBuffers samples (DirectX-Graphics-Samples,
// MIT licence) for the MinGW headers, to run under Wine on d3d12-metal: a window, a FLIP_DISCARD swap
// chain, the sample's resources and draws, a fence-synchronised render loop. The sample's own pixels
// are checked from a copy of the last back buffer.
//
//   hello_samples.exe texture        checkerboard texture, descriptor table SRV, static point/border sampler
//   hello_samples.exe constbuffers   triangle moved each frame through a mapped constant buffer
//   options: --frames N (default 120)
//
// Differences from the samples: precompiled DXIL shaders, no DirectXMath/d3dx12 (the few helper
// structures are written out), and the loop renders whenever no message is waiting.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "const_buffers_ps.h"
#include "const_buffers_vs.h"
#include "hello_texture_ps.h"
#include "hello_texture_vs.h"
#include "wine_test.h"

namespace {

constexpr UINT kFrameCount = 2;
constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;
constexpr float kClearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};
constexpr UINT kTextureSize = 256;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_PAINT) {
        ValidateRect(window, nullptr);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

class Sample {
public:
    Sample(bool texture, UINT frames) : texture_(texture), frames_(frames) {}
    int run(HINSTANCE instance);

private:
    void load();
    void load_texture_assets();
    void load_constant_buffer_assets();
    void render(bool capture);
    void wait_for_gpu();
    bool check_texture(const unsigned char *mapped, UINT pitch);
    bool check_constants(const unsigned char *mapped, UINT pitch);

    bool texture_;
    UINT frames_;
    HWND hwnd_ = nullptr;
    D3D12_VIEWPORT viewport_ = {0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor_ = {0, 0, LONG(kWidth), LONG(kHeight)};

    ComPtr<IDXGISwapChain3> swap_chain_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, shader_heap_;
    ComPtr<ID3D12Resource> targets_[kFrameCount];
    ComPtr<ID3D12RootSignature> signature_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12Resource> vertex_buffer_, texture_resource_, constant_buffer_, readback_;
    D3D12_VERTEX_BUFFER_VIEW vbv_ = {};
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 fence_value_ = 1;
    UINT frame_index_ = 0, rtv_size_ = 0, readback_pitch_ = 0;
    UINT frame_ = 0;
    float offset_ = 0.0f;
    float* constants_ = nullptr;  // mapped constant buffer (offset in the first float)
};

int Sample::run(HINSTANCE instance)
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"HelloSamples";
    RegisterClassExW(&window_class);
    RECT rect = {0, 0, LONG(kWidth), LONG(kHeight)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowW(window_class.lpszClassName, texture_ ? L"D3D12 Hello Texture" : L"D3D12 Hello ConstBuffers",
                          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                          rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    CHECK(hwnd_);
    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    load();

    bool ok = true;
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else if (hwnd_) {
            const bool last = frame_ + 1 == frames_;
            render(last);
            ++frame_;
            if (last) {
                unsigned char *mapped = nullptr;
                CHECK_HR(readback_->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
                ok = texture_ ? check_texture(mapped, readback_pitch_) : check_constants(mapped, readback_pitch_);
                readback_->Unmap(0, nullptr);
                std::printf("hello_samples %s: %s (%u frames)\n", texture_ ? "texture" : "constbuffers",
                            ok ? "PASS" : "FAIL", frame_);
                std::fflush(stdout);
                HWND window = hwnd_;
                hwnd_ = nullptr;
                DestroyWindow(window);
            }
        }
    }
    wait_for_gpu();
    CloseHandle(event_);
    return ok ? 0 : 1;
}

void Sample::load()
{
    ComPtr<IDXGIFactory4> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK_HR(factory->EnumAdapters1(0, &adapter));
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)));
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    CHECK_HR(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)));

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.BufferCount = kFrameCount;
    swap_chain_desc.Width = kWidth;
    swap_chain_desc.Height = kHeight;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swap_chain;
    CHECK_HR(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &swap_chain_desc, nullptr, nullptr, &swap_chain));
    CHECK_HR(swap_chain.As(&swap_chain_));
    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.NumDescriptors = kFrameCount;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    CHECK_HR(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)));
    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < kFrameCount; ++n) {
        CHECK_HR(swap_chain_->GetBuffer(n, IID_PPV_ARGS(&targets_[n])));
        device_->CreateRenderTargetView(targets_[n].Get(), nullptr, rtv);
        rtv.ptr += rtv_size_;
    }
    CHECK_HR(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)));

    D3D12_DESCRIPTOR_HEAP_DESC shader_desc = {};
    shader_desc.NumDescriptors = 1;
    shader_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shader_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK_HR(device_->CreateDescriptorHeap(&shader_desc, IID_PPV_ARGS(&shader_heap_)));

    CHECK_HR(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(event_);

    if (texture_)
        load_texture_assets();
    else
        load_constant_buffer_assets();
}

void Sample::load_texture_assets()
{
    // Root signature: an SRV table for the pixel shader and a static point sampler with border addressing.
    D3D12_DESCRIPTOR_RANGE1 range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    D3D12_ROOT_PARAMETER1 parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1 = {1, &parameter, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, error;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&desc, &blob, &error));
    CHECK_HR(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature_)));

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = default_pipeline_desc(signature_.Get(), layout, 2, g_hello_texture_vs,
                                                                   sizeof(g_hello_texture_vs), g_hello_texture_ps,
                                                                   sizeof(g_hello_texture_ps), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK_HR(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)));
    CHECK_HR(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), pso_.Get(), IID_PPV_ARGS(&list_)));

    // The sample's triangle, scaled up to fill more of the window.
    const float aspect = float(kWidth) / float(kHeight);
    const float vertices[] = {0.0f,  0.5f * aspect, 0.0f, 0.5f, 0.0f,   0.5f,  -0.5f * aspect, 0.0f, 1.0f,
                              1.0f,  -0.5f, -0.5f * aspect, 0.0f, 0.0f, 1.0f};
    const D3D12_HEAP_PROPERTIES upload = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC buffer = buffer_desc(sizeof(vertices));
    CHECK_HR(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&vertex_buffer_)));
    void *mapped = nullptr;
    CHECK_HR(vertex_buffer_->Map(0, nullptr, &mapped));
    std::memcpy(mapped, vertices, sizeof(vertices));
    vertex_buffer_->Unmap(0, nullptr);
    vbv_ = {vertex_buffer_->GetGPUVirtualAddress(), sizeof(vertices), 5 * sizeof(float)};

    // The checkerboard texture, filled through an upload buffer in the setup command list.
    const D3D12_HEAP_PROPERTIES default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC texture_description = texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kTextureSize, kTextureSize,
                                                                 D3D12_RESOURCE_FLAG_NONE);
    CHECK_HR(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_description,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture_resource_)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0, total = 0;
    device_->GetCopyableFootprints(&texture_description, 0, 1, 0, &footprint, &rows, &row_size, &total);
    buffer = buffer_desc(total);
    ComPtr<ID3D12Resource> staging;
    CHECK_HR(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&staging)));
    unsigned char *pixels = nullptr;
    CHECK_HR(staging->Map(0, nullptr, reinterpret_cast<void **>(&pixels)));
    for (UINT y = 0; y < kTextureSize; ++y) {
        for (UINT x = 0; x < kTextureSize; ++x) {
            const unsigned char value = ((x / 32) % 2 == (y / 32) % 2) ? 0x00 : 0xff;  // GenerateTextureData
            unsigned char *p = pixels + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch + x * 4;
            p[0] = p[1] = p[2] = value;
            p[3] = 0xff;
        }
    }
    staging->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = texture_resource_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = staging.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER barrier = transition(texture_resource_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(texture_resource_.Get(), &srv, shader_heap_->GetCPUDescriptorHandleForHeapStart());

    CHECK_HR(list_->Close());
    ID3D12CommandList *lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();
}

void Sample::load_constant_buffer_assets()
{
    D3D12_DESCRIPTOR_RANGE1 range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    D3D12_ROOT_PARAMETER1 parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1 = {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, error;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&desc, &blob, &error));
    CHECK_HR(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature_)));

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = default_pipeline_desc(signature_.Get(), layout, 2, g_const_buffers_vs,
                                                                   sizeof(g_const_buffers_vs), g_const_buffers_ps,
                                                                   sizeof(g_const_buffers_ps), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK_HR(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)));
    CHECK_HR(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), pso_.Get(), IID_PPV_ARGS(&list_)));
    CHECK_HR(list_->Close());

    const float aspect = float(kWidth) / float(kHeight);
    const float vertices[] = {0.0f,   0.25f * aspect, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                              0.25f,  -0.25f * aspect, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
                              -0.25f, -0.25f * aspect, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
    const D3D12_HEAP_PROPERTIES upload = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC buffer = buffer_desc(sizeof(vertices));
    CHECK_HR(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&vertex_buffer_)));
    void *mapped = nullptr;
    CHECK_HR(vertex_buffer_->Map(0, nullptr, &mapped));
    std::memcpy(mapped, vertices, sizeof(vertices));
    vertex_buffer_->Unmap(0, nullptr);
    vbv_ = {vertex_buffer_->GetGPUVirtualAddress(), sizeof(vertices), 7 * sizeof(float)};

    // The scene constant buffer (256 bytes: offset plus padding) stays mapped for the life of the program.
    const UINT constant_buffer_size = 256;
    buffer = buffer_desc(constant_buffer_size);
    CHECK_HR(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&constant_buffer_)));
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {constant_buffer_->GetGPUVirtualAddress(), constant_buffer_size};
    device_->CreateConstantBufferView(&cbv, shader_heap_->GetCPUDescriptorHandleForHeapStart());
    CHECK_HR(constant_buffer_->Map(0, nullptr, reinterpret_cast<void **>(&constants_)));
    std::memset(constants_, 0, constant_buffer_size);
}

void Sample::render(bool capture)
{
    if (!texture_) {
        // OnUpdate of the sample: move the triangle, wrapping at the bounds.
        offset_ += 0.005f;
        if (offset_ > 1.25f)
            offset_ = -1.25f;
        constants_[0] = offset_;
    }

    CHECK_HR(allocator_->Reset());
    CHECK_HR(list_->Reset(allocator_.Get(), pso_.Get()));
    list_->SetGraphicsRootSignature(signature_.Get());
    ID3D12DescriptorHeap *heaps[] = {shader_heap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRootDescriptorTable(0, shader_heap_->GetGPUDescriptorHandleForHeapStart());
    list_->RSSetViewports(1, &viewport_);
    list_->RSSetScissorRects(1, &scissor_);

    D3D12_RESOURCE_BARRIER barrier = transition(targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(frame_index_) * rtv_size_;
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list_->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->IASetVertexBuffers(0, 1, &vbv_);
    list_->DrawInstanced(3, 1, 0, 0);

    if (capture) {
        barrier = transition(targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->ResourceBarrier(1, &barrier);
        D3D12_RESOURCE_DESC desc = targets_[frame_index_]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        readback_pitch_ = footprint.Footprint.RowPitch;
        const D3D12_HEAP_PROPERTIES heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC readback_desc = buffer_desc(total);
        CHECK_HR(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)));
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = readback_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        src.pResource = targets_[frame_index_].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier = transition(targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        barrier = transition(targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PRESENT);
    }
    list_->ResourceBarrier(1, &barrier);
    CHECK_HR(list_->Close());
    ID3D12CommandList *lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    CHECK_HR(swap_chain_->Present(1, 0));
    wait_for_gpu();
    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
}

void Sample::wait_for_gpu()
{
    const UINT64 value = fence_value_++;
    CHECK_HR(queue_->Signal(fence_.Get(), value));
    if (fence_->GetCompletedValue() < value) {
        CHECK_HR(fence_->SetEventOnCompletion(value, event_));
        WaitForSingleObject(event_, INFINITE);
    }
}

Pixel pixel_at(const unsigned char *mapped, UINT pitch, UINT x, UINT y)
{
    Pixel p;
    std::memcpy(&p, mapped + size_t(y) * pitch + x * sizeof(Pixel), sizeof(p));
    return p;
}

// Texture coordinates are linear in x and y over the triangle (apex (0, 0.5 a) with uv (0.5, 0), base
// y = -0.5 a with u from 0 to 1 over x = -0.5 .. 0.5); away from the cell edges the checkerboard says black
// where the cell indices have the same parity.
bool Sample::check_texture(const unsigned char *mapped, UINT pitch)
{
    const float aspect = float(kWidth) / float(kHeight);
    int checked = 0, wrong = 0, black = 0, white = 0;
    for (UINT py = 0; py < kHeight; py += 3) {
        for (UINT px = 0; px < kWidth; px += 3) {
            const float x = (px + 0.5f) / kWidth * 2 - 1, y = 1 - (py + 0.5f) / kHeight * 2;
            const float v = (0.5f * aspect - y) / aspect;
            const float u = 0.5f + x;
            const bool inside = v > 0.08f && v < 0.97f && std::fabs(x) < 0.5f * v - 0.03f;
            const float fu = u * 8 - std::floor(u * 8), fv = v * 8 - std::floor(v * 8);
            if (!inside || fu < 0.2f || fu > 0.8f || fv < 0.2f || fv > 0.8f)
                continue;
            const bool is_black = int(u * 8) % 2 == int(v * 8) % 2;
            const Pixel p = pixel_at(mapped, pitch, px, py);
            ++checked;
            (is_black ? black : white)++;
            if (!near_pixel(p, is_black ? Pixel{0, 0, 0, 255} : Pixel{255, 255, 255, 255}))
                ++wrong;
        }
    }
    const Pixel corner = pixel_at(mapped, pitch, 2, 2);
    std::printf("hello_samples texture: %d texels checked (%d black, %d white), %d wrong; corner (%u,%u,%u)\n", checked,
                black, white, wrong, corner.r, corner.g, corner.b);
    return checked > 200 && black > 30 && white > 30 && wrong == 0 && near_pixel(corner, {0, 51, 102, 255});
}

// By the last frame the triangle has moved by (frames * 0.005) and its centroid shows the average of the
// vertex colours.
bool Sample::check_constants(const unsigned char *mapped, UINT pitch)
{
    const float aspect = float(kWidth) / float(kHeight);
    const float cx_ndc = offset_, cy_ndc = -0.25f * aspect / 3.0f;
    const UINT cx = UINT((cx_ndc + 1) / 2 * kWidth), cy = UINT((1 - cy_ndc) / 2 * kHeight);
    const Pixel centroid = pixel_at(mapped, pitch, cx, cy);
    const Pixel corner = pixel_at(mapped, pitch, 2, 2);
    std::printf("hello_samples constbuffers: offset %.3f, centroid (%u,%u,%u), corner (%u,%u,%u)\n", offset_, centroid.r,
                centroid.g, centroid.b, corner.r, corner.g, corner.b);
    return near_pixel(centroid, {85, 85, 85, 255}, 10) && near_pixel(corner, {0, 51, 102, 255}) && offset_ > 0.0f;
}

} // namespace

int main(int argc, char **argv)
{
    bool texture = true;
    UINT frames = 120;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "texture"))
            texture = true;
        else if (!std::strcmp(argv[i], "constbuffers"))
            texture = false;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = static_cast<UINT>(std::atoi(argv[++i]));
        else {
            std::fprintf(stderr, "usage: hello_samples (texture|constbuffers) [--frames N]\n");
            return 2;
        }
    }
    Sample sample(texture, frames);
    return sample.run(GetModuleHandleW(nullptr));
}
