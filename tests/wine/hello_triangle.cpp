// SPDX-License-Identifier: LGPL-2.1-or-later
// A port of Microsoft's D3D12HelloTriangle sample (DirectX-Graphics-Samples, MIT
// licence) for the MinGW headers, built to run under Wine on d3d12-metal. The
// structure and the D3D12 calls follow the sample: window, FLIP_DISCARD swap
// chain of two buffers, an RTV heap, an empty root signature, a PSO, a vertex
// buffer in an upload heap, and a fence-synchronised render loop.
//
// Differences from the sample:
//  - The shaders are DXIL compiled by DXC at build time (embedded headers); the
//    sample compiles HLSL to DXBC at run time, which this layer does not accept.
//  - No DirectXMath or d3dx12: the few helper structures are written out.
//  - The loop renders whenever no message is waiting, for --frames N frames
//    (0 = until the window closes), and reports the frame rate.
//  - --selftest copies the last back buffer to a readback buffer and checks it.
//
// Usage: hello_triangle.exe [--frames N] [--selftest]
#include <cstdint>
#include <cstring>
#include <string>

#include "triangle_ps.h"
#include "triangle_vs.h"
#include "wine_test.h"

namespace {

constexpr UINT kFrameCount = 2;
constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr float kClearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};

struct Vertex {
    float position[3];
    float color[4];
};

struct Options {
    UINT frames = 0;
    bool selftest = false;
};

class HelloTriangle {
public:
    explicit HelloTriangle(const Options &options) : options_(options) {}

    void run(HINSTANCE instance);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void on_init();
    void load_pipeline();
    void load_assets();
    void on_render();
    void populate_command_list(bool capture);
    void wait_for_previous_frame();
    bool check_capture();
    void report_window();

    Options options_;
    HWND hwnd_ = nullptr;
    float aspect_ratio_ = float(kWidth) / float(kHeight);

    // Pipeline objects.
    D3D12_VIEWPORT viewport_ = {0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor_rect_ = {0, 0, LONG(kWidth), LONG(kHeight)};
    ComPtr<IDXGISwapChain3> swap_chain_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Resource> render_targets_[kFrameCount];
    ComPtr<ID3D12CommandAllocator> command_allocator_;
    ComPtr<ID3D12CommandQueue> command_queue_;
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12PipelineState> pipeline_state_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    UINT rtv_descriptor_size_ = 0;

    // App resources.
    ComPtr<ID3D12Resource> vertex_buffer_;
    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view_ = {};

    // Synchronization objects.
    UINT frame_index_ = 0;
    HANDLE fence_event_ = nullptr;
    ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_ = 0;

    // Run state.
    UINT frame_ = 0;
    LARGE_INTEGER start_time_ = {};
    bool failed_ = false;
    ComPtr<ID3D12Resource> readback_;
    UINT readback_row_pitch_ = 0;
};

void get_hardware_adapter(IDXGIFactory1 *factory, IDXGIAdapter1 **adapter_out)
{
    *adapter_out = nullptr;
    ComPtr<IDXGIAdapter1> adapter;

    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) {
        for (UINT index = 0; SUCCEEDED(factory6->EnumAdapterByGpuPreference(
                 index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
             ++index) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
                break;
        }
    }

    if (adapter.Get() == nullptr) {
        for (UINT index = 0; SUCCEEDED(factory->EnumAdapters1(index, &adapter)); ++index) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
                break;
        }
    }
    *adapter_out = adapter.Detach();
}

void HelloTriangle::run(HINSTANCE instance)
{
    // Initialize the window class.
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = L"DXSampleClass";
    RegisterClassExW(&window_class);

    RECT window_rect = {0, 0, LONG(kWidth), LONG(kHeight)};
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

    // Create the window and store a handle to it.
    hwnd_ = CreateWindowW(window_class.lpszClassName, L"D3D12 Hello Triangle", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                          CW_USEDEFAULT, window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
                          nullptr, nullptr, instance, this);
    CHECK(hwnd_);
    ShowWindow(hwnd_, SW_SHOWDEFAULT);

    on_init();

    // Main loop: handle messages, render when there are none.
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else if (hwnd_) {
            on_render();
        }
    }

    // Ensure that the GPU is no longer referencing resources that are about to be cleaned up.
    wait_for_previous_frame();
    CloseHandle(fence_event_);
    if (failed_)
        std::exit(1);
}

LRESULT CALLBACK HelloTriangle::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT:
        ValidateRect(window, nullptr);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void HelloTriangle::on_init()
{
    load_pipeline();
    load_assets();
}

// Load the rendering pipeline dependencies.
void HelloTriangle::load_pipeline()
{
    ComPtr<IDXGIFactory4> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIAdapter1> hardware_adapter;
    get_hardware_adapter(factory.Get(), &hardware_adapter);
    CHECK(hardware_adapter);
    CHECK_HR(D3D12CreateDevice(hardware_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)));

    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_HR(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_)));

    // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.BufferCount = kFrameCount;
    swap_chain_desc.Width = kWidth;
    swap_chain_desc.Height = kHeight;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swap_chain;
    CHECK_HR(factory->CreateSwapChainForHwnd(command_queue_.Get(), hwnd_, &swap_chain_desc, nullptr, nullptr,
                                             &swap_chain));

    // This sample does not support fullscreen transitions.
    CHECK_HR(factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER));

    CHECK_HR(swap_chain.As(&swap_chain_));
    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    // Create descriptor heaps: a render target view (RTV) descriptor heap.
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = kFrameCount;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK_HR(device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap_)));
    rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create frame resources: a RTV for each frame.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < kFrameCount; n++) {
        CHECK_HR(swap_chain_->GetBuffer(n, IID_PPV_ARGS(&render_targets_[n])));
        device_->CreateRenderTargetView(render_targets_[n].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += rtv_descriptor_size_;
    }

    CHECK_HR(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator_)));
}

// Load the sample assets.
void HelloTriangle::load_assets()
{
    // Create an empty root signature.
    {
        D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
        root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        CHECK_HR(D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        CHECK_HR(device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                              IID_PPV_ARGS(&root_signature_)));
    }

    // Create the pipeline state; the shaders are precompiled DXIL.
    {
        // Define the vertex input layout.
        D3D12_INPUT_ELEMENT_DESC input_element_descs[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

        // Describe and create the graphics pipeline state object (PSO).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc =
            default_pipeline_desc(root_signature_.Get(), input_element_descs, _countof(input_element_descs),
                                  g_triangle_vs, sizeof(g_triangle_vs), g_triangle_ps, sizeof(g_triangle_ps),
                                  DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK_HR(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state_)));
    }

    // Create the command list.
    CHECK_HR(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator_.Get(),
                                        pipeline_state_.Get(), IID_PPV_ARGS(&command_list_)));

    // Command lists are created in the recording state, but there is nothing
    // to record yet. The main loop expects it to be closed, so close it now.
    CHECK_HR(command_list_->Close());

    // Create the vertex buffer.
    {
        // Define the geometry for a triangle.
        Vertex triangle_vertices[] = {{{0.0f, 0.25f * aspect_ratio_, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                                      {{0.25f, -0.25f * aspect_ratio_, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
                                      {{-0.25f, -0.25f * aspect_ratio_, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}};
        const UINT vertex_buffer_size = sizeof(triangle_vertices);

        // Note: using upload heaps to transfer static data like vert buffers is not
        // recommended; it is used here for simplicity, as in the original sample.
        const D3D12_HEAP_PROPERTIES heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC desc = buffer_desc(vertex_buffer_size);
        CHECK_HR(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&vertex_buffer_)));

        // Copy the triangle data to the vertex buffer.
        UINT8 *vertex_data_begin;
        D3D12_RANGE read_range = {0, 0};  // We do not intend to read from this resource on the CPU.
        CHECK_HR(vertex_buffer_->Map(0, &read_range, reinterpret_cast<void **>(&vertex_data_begin)));
        std::memcpy(vertex_data_begin, triangle_vertices, sizeof(triangle_vertices));
        vertex_buffer_->Unmap(0, nullptr);

        // Initialize the vertex buffer view.
        vertex_buffer_view_.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
        vertex_buffer_view_.StrideInBytes = sizeof(Vertex);
        vertex_buffer_view_.SizeInBytes = vertex_buffer_size;
    }

    // Create synchronization objects and wait until assets have been uploaded to the GPU.
    {
        CHECK_HR(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
        fence_value_ = 1;

        // Create an event handle to use for frame synchronization.
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(fence_event_);

        // Wait for the command list to execute; we are reusing the same command
        // list in our main loop but for now, we just want to wait for setup to
        // complete before continuing.
        wait_for_previous_frame();
    }
}

// Render the scene.
void HelloTriangle::on_render()
{
    if (frame_ == 1)
        QueryPerformanceCounter(&start_time_);

    const bool last = options_.frames != 0 && frame_ + 1 == options_.frames;

    // Record all the commands we need to render the scene into the command list.
    populate_command_list(last && options_.selftest);

    // Execute the command list.
    ID3D12CommandList *command_lists[] = {command_list_.Get()};
    command_queue_->ExecuteCommandLists(_countof(command_lists), command_lists);

    // Present the frame.
    CHECK_HR(swap_chain_->Present(1, 0));

    wait_for_previous_frame();
    ++frame_;

    if (frame_ == 30)
        report_window();

    if (last) {
        LARGE_INTEGER end, frequency;
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&frequency);
        const double seconds = double(end.QuadPart - start_time_.QuadPart) / double(frequency.QuadPart);
        std::printf("hello_triangle: %u frames, %.2f s (frames 2..%u), %.1f fps\n", frame_, seconds, frame_,
                    seconds > 0 ? double(frame_ - 1) / seconds : 0.0);
        if (options_.selftest) {
            if (check_capture())
                std::printf("hello_triangle: selftest PASS\n");
            else {
                std::printf("hello_triangle: selftest FAIL\n");
                failed_ = true;
            }
        }
        std::fflush(stdout);
        HWND window = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(window);  // WM_DESTROY posts WM_QUIT
    }
}

void HelloTriangle::populate_command_list(bool capture)
{
    // Command list allocators can only be reset when the associated
    // command lists have finished execution on the GPU; apps should use
    // fences to determine GPU execution progress.
    CHECK_HR(command_allocator_->Reset());

    // However, when ExecuteCommandList() is called on a particular command
    // list, that command list can then be reset at any time and must be before
    // re-recording.
    CHECK_HR(command_list_->Reset(command_allocator_.Get(), pipeline_state_.Get()));

    // Set necessary state.
    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_rect_);

    // Indicate that the back buffer will be used as a render target.
    D3D12_RESOURCE_BARRIER barrier = transition(render_targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    command_list_->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv_handle.ptr += SIZE_T(frame_index_) * rtv_descriptor_size_;
    command_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    // Record commands.
    command_list_->ClearRenderTargetView(rtv_handle, kClearColor, 0, nullptr);
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->IASetVertexBuffers(0, 1, &vertex_buffer_view_);
    command_list_->DrawInstanced(3, 1, 0, 0);

    if (capture) {
        // Selftest: copy what was just drawn into a readback buffer.
        D3D12_RESOURCE_BARRIER to_copy = transition(render_targets_[frame_index_].Get(),
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list_->ResourceBarrier(1, &to_copy);

        D3D12_RESOURCE_DESC desc = render_targets_[frame_index_]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        readback_row_pitch_ = footprint.Footprint.RowPitch;
        const D3D12_HEAP_PROPERTIES heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC readback_desc = buffer_desc(total);
        CHECK_HR(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&readback_)));
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = render_targets_[frame_index_].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        command_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barrier = transition(render_targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_PRESENT);
    } else {
        // Indicate that the back buffer will now be used to present.
        barrier = transition(render_targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PRESENT);
    }
    command_list_->ResourceBarrier(1, &barrier);

    CHECK_HR(command_list_->Close());
}

void HelloTriangle::wait_for_previous_frame()
{
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity, as in the original sample.

    // Signal and increment the fence value.
    const UINT64 fence = fence_value_;
    CHECK_HR(command_queue_->Signal(fence_.Get(), fence));
    fence_value_++;

    // Wait until the previous frame is finished.
    if (fence_->GetCompletedValue() < fence) {
        CHECK_HR(fence_->SetEventOnCompletion(fence, fence_event_));
        WaitForSingleObject(fence_event_, INFINITE);
    }

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
}

// Checks the captured frame: the corner shows the clear colour, the centre the
// interpolated vertex colours (0.5, 0.25, 0.25 by symmetry of the triangle).
bool HelloTriangle::check_capture()
{
    unsigned char *mapped = nullptr;
    CHECK_HR(readback_->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
    auto pixel = [&](UINT x, UINT y) {
        Pixel p;
        std::memcpy(&p, mapped + size_t(y) * readback_row_pitch_ + x * sizeof(Pixel), sizeof(p));
        return p;
    };
    const Pixel center = pixel(kWidth / 2, kHeight / 2);
    const Pixel corner = pixel(0, 0);
    readback_->Unmap(0, nullptr);

    const Pixel expected_center = {128, 64, 64, 255};
    const Pixel expected_corner = {0, 51, 102, 255};
    std::printf("hello_triangle: center (%u,%u,%u,%u) expected ~(%u,%u,%u,%u)\n", center.r, center.g, center.b,
                center.a, expected_center.r, expected_center.g, expected_center.b, expected_center.a);
    std::printf("hello_triangle: corner (%u,%u,%u,%u) expected (%u,%u,%u,%u)\n", corner.r, corner.g, corner.b,
                corner.a, expected_corner.r, expected_corner.g, expected_corner.b, expected_corner.a);
    return near_pixel(center, expected_center, 6) && near_pixel(corner, expected_corner, 2)
           && !near_pixel(center, expected_corner, 16);
}

// Prints where the client area is on screen, for the screenshot check.
void HelloTriangle::report_window()
{
    POINT origin = {0, 0};
    ClientToScreen(hwnd_, &origin);
    RECT client;
    GetClientRect(hwnd_, &client);
    std::printf("hello_triangle: WINDOW client_screen_rect=%ld,%ld,%ld,%ld\n", origin.x, origin.y,
                client.right - client.left, client.bottom - client.top);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--selftest"))
            options.selftest = true;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
            options.frames = static_cast<UINT>(std::atoi(argv[++i]));
        else {
            std::fprintf(stderr, "usage: hello_triangle [--frames N] [--selftest]\n");
            return 2;
        }
    }
    if (options.selftest && options.frames == 0)
        options.frames = 120;

    HelloTriangle sample(options);
    sample.run(GetModuleHandleW(nullptr));
    return 0;
}
