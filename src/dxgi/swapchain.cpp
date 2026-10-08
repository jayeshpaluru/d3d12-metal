#include "dxgi/swapchain.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "common/com.h"
#include "common/log.h"
#include "common/platform.h"
#include "common/private_data.h"
#include "common/stats.h"
#include "d3d12/command_queue.h"
#include "d3d12/device.h"
#include "d3d12/fence.h"
#include "d3d12/formats.h"
#include "d3d12/resource.h"
#include "dxgi/output.h"

namespace d3d12m {

namespace {

constexpr UINT kMaxBuffers = 16;

uint64_t window_id(HWND window)
{
    return static_cast<uint64_t>((uintptr_t)window);
}

bool is_flip(DXGI_SWAP_EFFECT effect)
{
    return effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
}

// The state behind the frame latency waitable object: a manual-reset event that is
// signaled while fewer than `maximum` presented frames are still on the GPU. Shared
// with the fence callbacks that finish frames, which may outlive the swap chain.
class Latency {
public:
    explicit Latency(HANDLE event) : event_(event) {}
    ~Latency() { platform_close_event(event_); }

    HANDLE event() const { return event_; }
    HANDLE duplicate_event() const { return platform_duplicate_event(event_); }

    void frame_presented()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++in_flight_;
        update();
    }

    void frame_finished()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_)
            --in_flight_;
        update();
    }

    void set_maximum(UINT maximum)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        maximum_ = maximum;
        update();
    }

    UINT maximum()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return maximum_;
    }

private:
    // Each event call is a round trip to the Wine server: only when the state flips.
    void update()
    {
        const bool ready = in_flight_ < maximum_;
        if (ready == signaled_)
            return;
        signaled_ = ready;
        if (ready)
            platform_set_event(event_);
        else
            platform_reset_event(event_);
    }

    HANDLE event_;
    bool signaled_ = true;  // created signaled
    std::mutex mutex_;
    UINT in_flight_ = 0;
    UINT maximum_ = 1;
};

class SwapChain final : public WithPrivateData<RefCounted<IDXGISwapChain4>> {
public:
    static HRESULT create(IDXGIFactory *factory, IUnknown *queue_unknown, HWND window, const DXGI_SWAP_CHAIN_DESC1 &desc,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, SwapChain **out)
    {
        // Only this layer's queues present: the buffers must be its textures.
        ID3D12CommandQueue *as_queue = nullptr;
        if (!queue_unknown || FAILED(queue_unknown->QueryInterface(__uuidof(ID3D12CommandQueue),
                                                                   reinterpret_cast<void **>(&as_queue))))
            return DXGI_ERROR_INVALID_CALL;
        // The application may pass a wrapper that forwards to one of our queues.
        auto *queue = ours<CommandQueue>(as_queue);
        if (!queue) {
            as_queue->Release();
            return DXGI_ERROR_INVALID_CALL;
        }
        queue->AddRef();
        as_queue->Release();

        auto *swap_chain = new SwapChain(factory, queue);
        swap_chain->window_ = window;
        swap_chain->desc_ = desc;
        if (fullscreen)
            swap_chain->fullscreen_ = *fullscreen;
        else
            swap_chain->fullscreen_.Windowed = TRUE;
        HRESULT hr = swap_chain->init();
        if (FAILED(hr)) {
            swap_chain->Release();
            return hr;
        }
        *out = swap_chain;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, IDXGIObject, IDXGIDeviceSubObject, IDXGISwapChain, IDXGISwapChain1,
                                IDXGISwapChain2, IDXGISwapChain3, IDXGISwapChain4>(this, riid, out);
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override
    {
        D3D12M_TRACED_BEGIN
        return factory_->QueryInterface(riid, parent);
        D3D12M_TRACED_END(riid, parent)
    }

    // IDXGIDeviceSubObject: the device of a D3D12 swap chain is its command queue.
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override
    {
        D3D12M_TRACED_BEGIN
        return queue_->QueryInterface(riid, device);
        D3D12M_TRACED_END(riid, device)
    }

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) override
    {
        D3D12M_TRACED_BEGIN
        return present(sync_interval, flags);
        D3D12M_TRACED_END(sync_interval, flags)
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void **surface) override
    {
        D3D12M_TRACED_BEGIN
        if (!surface)
            return DXGI_ERROR_INVALID_CALL;
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= buffers_.size())
            return DXGI_ERROR_INVALID_CALL;
        return buffers_[index]->QueryInterface(riid, surface);
        D3D12M_TRACED_END(index, riid, surface)
    }

    // Windowed only: a request for fullscreen is accepted and ignored.
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput *) override { D3D12M_TRACED_BEGIN return S_OK; D3D12M_TRACED_END() }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen, IDXGIOutput **target) override
    {
        D3D12M_TRACED_BEGIN
        if (fullscreen)
            *fullscreen = FALSE;
        if (target)
            *target = nullptr;
        return S_OK;
        D3D12M_TRACED_END(fullscreen, target)
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc)
            return DXGI_ERROR_INVALID_CALL;
        std::lock_guard<std::mutex> lock(mutex_);
        desc->BufferDesc.Width = desc_.Width;
        desc->BufferDesc.Height = desc_.Height;
        desc->BufferDesc.RefreshRate = fullscreen_.RefreshRate;
        desc->BufferDesc.Format = desc_.Format;
        desc->BufferDesc.ScanlineOrdering = fullscreen_.ScanlineOrdering;
        desc->BufferDesc.Scaling = fullscreen_.Scaling;
        desc->SampleDesc = desc_.SampleDesc;
        desc->BufferUsage = desc_.BufferUsage;
        desc->BufferCount = desc_.BufferCount;
        desc->OutputWindow = window_;
        desc->Windowed = fullscreen_.Windowed;
        desc->SwapEffect = desc_.SwapEffect;
        desc->Flags = desc_.Flags;
        return S_OK;
        D3D12M_TRACED_END(desc)
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override
    {
        D3D12M_TRACED_BEGIN
        return resize(count, width, height, format, flags);
        D3D12M_TRACED_END(count, width, height, format, flags)
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *) override { D3D12M_TRACED_BEGIN return S_OK; D3D12M_TRACED_END() }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override
    {
        D3D12M_TRACED_BEGIN
        if (!output)
            return DXGI_ERROR_INVALID_CALL;
        IDXGIAdapter *adapter = nullptr;
        if (FAILED(factory_->EnumAdapters(0, &adapter))) {
            *output = nullptr;
            return DXGI_ERROR_NOT_FOUND;
        }
        IDXGIOutput6 *created = create_output(adapter);
        adapter->Release();
        return hand_out(created, __uuidof(IDXGIOutput), reinterpret_cast<void **>(output));
        D3D12M_TRACED_END(output)
    }

    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) override
    {
        D3D12M_TRACED_BEGIN
        if (!stats)
            return DXGI_ERROR_INVALID_CALL;
        *stats = {};
        stats->PresentCount = present_count_;
        return S_OK;
        D3D12M_TRACED_END(stats)
    }

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *count) override
    {
        D3D12M_TRACED_BEGIN
        if (!count)
            return DXGI_ERROR_INVALID_CALL;
        *count = present_count_;
        return S_OK;
        D3D12M_TRACED_END(count)
    }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *desc) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc)
            return DXGI_ERROR_INVALID_CALL;
        std::lock_guard<std::mutex> lock(mutex_);
        *desc = desc_;
        return S_OK;
        D3D12M_TRACED_END(desc)
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc)
            return DXGI_ERROR_INVALID_CALL;
        *desc = fullscreen_;
        return S_OK;
        D3D12M_TRACED_END(desc)
    }

    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *window) override
    {
        D3D12M_TRACED_BEGIN
        if (!window)
            return DXGI_ERROR_INVALID_CALL;
        *window = window_;
        return S_OK;
        D3D12M_TRACED_END(window)
    }

    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void **window) override
    {
        D3D12M_TRACED_BEGIN
        if (window)
            *window = nullptr;
        return DXGI_ERROR_INVALID_CALL;
        D3D12M_TRACED_END(window)
    }

    HRESULT STDMETHODCALLTYPE Present1(UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS *) override
    {
        D3D12M_TRACED_BEGIN
        return present(sync_interval, flags);
        D3D12M_TRACED_END(sync_interval, flags)
    }

    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { D3D12M_TRACE(); return FALSE; }

    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **output) override
    {
        D3D12M_TRACED_BEGIN
        if (output)
            *output = nullptr;
        return S_OK;
        D3D12M_TRACED_END(output)
    }

    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *) override { D3D12M_TRACED_BEGIN return S_OK; D3D12M_TRACED_END() }

    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *color) override
    {
        D3D12M_TRACED_BEGIN
        if (!color)
            return DXGI_ERROR_INVALID_CALL;
        *color = {0, 0, 0, 1};
        return S_OK;
        D3D12M_TRACED_END(color)
    }

    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override
    {
        D3D12M_TRACED_BEGIN
        return rotation == DXGI_MODE_ROTATION_IDENTITY || rotation == DXGI_MODE_ROTATION_UNSPECIFIED
                   ? S_OK
                   : DXGI_ERROR_INVALID_CALL;
        D3D12M_TRACED_END(rotation)
    }

    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *rotation) override
    {
        D3D12M_TRACED_BEGIN
        if (!rotation)
            return DXGI_ERROR_INVALID_CALL;
        *rotation = DXGI_MODE_ROTATION_IDENTITY;
        return S_OK;
        D3D12M_TRACED_END(rotation)
    }

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT, UINT) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *width, UINT *height) override
    {
        D3D12M_TRACED_BEGIN
        if (!width || !height)
            return DXGI_ERROR_INVALID_CALL;
        std::lock_guard<std::mutex> lock(mutex_);
        *width = desc_.Width;
        *height = desc_.Height;
        return S_OK;
        D3D12M_TRACED_END(width, height)
    }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override
    {
        D3D12M_TRACED_BEGIN
        if (!latency_ || latency == 0 || latency > kMaxBuffers)
            return DXGI_ERROR_INVALID_CALL;
        latency_->set_maximum(latency);
        return S_OK;
        D3D12M_TRACED_END(latency)
    }

    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *latency) override
    {
        D3D12M_TRACED_BEGIN
        if (!latency || !latency_)
            return DXGI_ERROR_INVALID_CALL;
        *latency = latency_->maximum();
        return S_OK;
        D3D12M_TRACED_END(latency)
    }

    // The event is signaled while fewer than the maximum latency's number of
    // presented frames are on the GPU, and starts signaled. Each call returns a
    // new handle to it.
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
    {
        D3D12M_TRACE();
        return latency_ ? latency_->duplicate_event() : nullptr;
    }

    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *) override { D3D12M_STUB_HR(); }

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
    {
        D3D12M_TRACE();
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE color_space, UINT *support) override
    {
        D3D12M_TRACED_BEGIN
        if (!support)
            return DXGI_ERROR_INVALID_CALL;
        *support = color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                       ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
                       : 0;
        return S_OK;
        D3D12M_TRACED_END(color_space, support)
    }

    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color_space) override
    {
        D3D12M_TRACED_BEGIN
        return color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? S_OK : E_INVALIDARG;
        D3D12M_TRACED_END(color_space)
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                             const UINT *, IUnknown *const *) override
    {
        D3D12M_TRACED_BEGIN
        return resize(count, width, height, format, flags);
        D3D12M_TRACED_END(count, width, height, format, flags)
    }

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE, UINT, void *) override { D3D12M_STUB_HR(); }

private:
    SwapChain(IDXGIFactory *factory, CommandQueue *queue) : factory_(factory), queue_(queue)
    {
        factory_->AddRef();
    }

    ~SwapChain() override
    {
        release_buffers();
        if (handle_)
            mtlb_swapchain_destroy(handle_);
        safe_release(fence_);
        queue_->Release();
        factory_->Release();
    }

    HRESULT init()
    {
        if (window_ && (desc_.Width == 0 || desc_.Height == 0)) {
            UINT width = 0, height = 0;
            if (platform_window_client_size(window_, &width, &height)) {
                desc_.Width = desc_.Width ? desc_.Width : width;
                desc_.Height = desc_.Height ? desc_.Height : height;
            }
        }
        if (desc_.BufferCount == 0)
            desc_.BufferCount = 1;
        if (desc_.Width == 0 || desc_.Height == 0 || desc_.BufferCount > kMaxBuffers || desc_.SampleDesc.Count > 1
            || desc_.Stereo)
            return DXGI_ERROR_INVALID_CALL;

        mtlb_swapchain_desc native{};
        native.window = window_id(platform_root_window(window_));
        native.width = desc_.Width;
        native.height = desc_.Height;
        native.format = to_mtlb_format(desc_.Format);
        native.buffer_count = desc_.BufferCount;
        mtlb_result result = mtlb_swapchain_create(queue_->device()->handle(), &native, &handle_);
        if (result != MTLB_OK) {
            D3D12M_LOG("swap chain creation failed: %s", mtlb_last_error());
            return result == MTLB_ERROR_UNSUPPORTED ? DXGI_ERROR_UNSUPPORTED : to_hresult(result);
        }
        if (HRESULT hr = create_buffers(); FAILED(hr))
            return hr;

        if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
            HANDLE event = platform_create_event(true);
            if (!event)
                return E_FAIL;
            latency_ = std::make_shared<Latency>(event);
            void *fence = nullptr;
            if (FAILED(Fence::create(queue_->device(), 0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence)))
                return E_FAIL;
            fence_ = static_cast<ID3D12Fence *>(fence);
        }
        return S_OK;
    }

    // Callers hold mutex_ (or are constructing).
    HRESULT create_buffers()
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc_.Width;
        desc.Height = desc_.Height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = desc_.Format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        std::vector<ID3D12Resource *> created;
        for (UINT i = 0; i < desc_.BufferCount; ++i) {
            void *resource = nullptr;
            HRESULT hr = Resource::create_committed(queue_->device(), heap, desc, __uuidof(ID3D12Resource), &resource);
            if (FAILED(hr)) {
                for (ID3D12Resource *r : created)
                    r->Release();
                return hr;
            }
            created.push_back(static_cast<ID3D12Resource *>(resource));
        }
        release_buffers();
        buffers_ = std::move(created);
        current_ = 0;
        return S_OK;
    }

    void release_buffers()
    {
        for (ID3D12Resource *buffer : buffers_)
            buffer->Release();
        buffers_.clear();
    }

    HRESULT present(UINT sync_interval, UINT flags)
    {
        if (flags & DXGI_PRESENT_TEST)
            return S_OK;
        if (sync_interval > 4)
            return DXGI_ERROR_INVALID_CALL;
        // The bridge call can wait for a drawable: not under the lock.
        Resource *buffer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            buffer = static_cast<Resource *>(buffers_[current_]);
            buffer->AddRef();
            if (is_flip(desc_.SwapEffect))
                current_ = (current_ + 1) % static_cast<UINT>(buffers_.size());
        }
        stats_frame();
        trace_frame();
        mtlb_result result;
        {
            StatTimer timer(Stat::PresentNanos);
            result = mtlb_queue_present(queue_->handle(), handle_, buffer->texture(), sync_interval);
        }
        buffer->Release();
        if (result != MTLB_OK) {
            D3D12M_LOG("present failed: %s", mtlb_last_error());
            return to_hresult(result);
        }
        ++present_count_;
        if (latency_)
            track_frame_latency();
        return S_OK;
    }

    // Counts the frame just presented as in flight until the GPU has passed a fence
    // signalled behind it.
    void track_frame_latency()
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        const UINT64 value = ++fence_value_;
        std::shared_ptr<Latency> latency = latency_;
        latency->frame_presented();
        const mtlb_result signalled = mtlb_queue_signal(queue_->handle(), static_cast<Fence *>(fence_)->event(), value);
        const HRESULT waiting =
            signalled == MTLB_OK ? queue_->device()->fence_waiter().add(static_cast<Fence *>(fence_), value,
                                                                         [latency] { latency->frame_finished(); })
                                 : to_hresult(signalled);
        if (FAILED(waiting)) {
            // Never leave the application waiting on a frame nobody will finish.
            D3D12M_LOG("frame latency tracking failed: 0x%08x", static_cast<unsigned>(waiting));
            latency->frame_finished();
        }
    }

    HRESULT resize(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        DXGI_SWAP_CHAIN_DESC1 saved = desc_;
        if (count)
            desc_.BufferCount = count;
        if (format != DXGI_FORMAT_UNKNOWN)
            desc_.Format = format;
        if (width == 0 || height == 0) {
            UINT client_width = 0, client_height = 0;
            if (window_ && platform_window_client_size(window_, &client_width, &client_height)) {
                width = width ? width : client_width;
                height = height ? height : client_height;
            }
        }
        if (width)
            desc_.Width = width;
        if (height)
            desc_.Height = height;
        // The flags are replaced, except that the latency object cannot appear or go away.
        desc_.Flags = (flags & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
                      | (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (desc_.BufferCount > kMaxBuffers || desc_.Width == 0 || desc_.Height == 0) {
            desc_ = saved;
            return DXGI_ERROR_INVALID_CALL;
        }

        mtlb_result result = mtlb_swapchain_resize(handle_, desc_.Width, desc_.Height, to_mtlb_format(desc_.Format));
        HRESULT hr = result == MTLB_OK ? create_buffers() : to_hresult(result);
        if (FAILED(hr)) {
            desc_ = saved;
            return hr;
        }
        return S_OK;
    }

    IDXGIFactory *factory_;
    CommandQueue *queue_;
    HWND window_ = {};
    DXGI_SWAP_CHAIN_DESC1 desc_{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_{};
    mtlb_swapchain handle_ = 0;

    std::mutex mutex_;  // guards the buffers, their count and size, and the current index
    std::vector<ID3D12Resource *> buffers_;
    UINT current_ = 0;
    std::atomic<UINT> present_count_{0};

    // Frame latency waitable object (see GetFrameLatencyWaitableObject): a fence
    // signalled behind each presented frame tells when the frame has finished.
    std::shared_ptr<Latency> latency_;
    ID3D12Fence *fence_ = nullptr;
    std::mutex latency_mutex_;  // keeps fence values in step with the order of signals
    UINT64 fence_value_ = 0;
};

} // namespace

HRESULT create_swap_chain(IDXGIFactory *factory, IUnknown *queue, HWND window, const DXGI_SWAP_CHAIN_DESC1 &desc,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGISwapChain1 **out)
{
    if (!out)
        return DXGI_ERROR_INVALID_CALL;
    *out = nullptr;
    SwapChain *swap_chain = nullptr;
    if (HRESULT hr = SwapChain::create(factory, queue, window, desc, fullscreen, &swap_chain); FAILED(hr))
        return hr;
    *out = swap_chain;
    return S_OK;
}

HRESULT create_swap_chain(IDXGIFactory *factory, IUnknown *queue, const DXGI_SWAP_CHAIN_DESC &desc,
                          IDXGISwapChain **out)
{
    if (!out)
        return DXGI_ERROR_INVALID_CALL;
    DXGI_SWAP_CHAIN_DESC1 desc1{};
    desc1.Width = desc.BufferDesc.Width;
    desc1.Height = desc.BufferDesc.Height;
    desc1.Format = desc.BufferDesc.Format;
    desc1.SampleDesc = desc.SampleDesc;
    desc1.BufferUsage = desc.BufferUsage;
    desc1.BufferCount = desc.BufferCount;
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.SwapEffect = desc.SwapEffect;
    desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Flags = desc.Flags;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
    fullscreen.RefreshRate = desc.BufferDesc.RefreshRate;
    fullscreen.ScanlineOrdering = desc.BufferDesc.ScanlineOrdering;
    fullscreen.Scaling = desc.BufferDesc.Scaling;
    fullscreen.Windowed = desc.Windowed;

    IDXGISwapChain1 *created = nullptr;
    if (HRESULT hr = create_swap_chain(factory, queue, desc.OutputWindow, desc1, &fullscreen, &created); FAILED(hr)) {
        *out = nullptr;
        return hr;
    }
    *out = created;
    return S_OK;
}

} // namespace d3d12m
