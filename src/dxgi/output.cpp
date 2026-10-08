#include "dxgi/output.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <new>
#include <vector>

#include "common/com.h"
#include "common/log.h"
#include "common/private_data.h"

namespace d3d12m {

namespace {

struct Mode {
    UINT width, height, refresh_hz;

    bool operator<(const Mode &o) const
    {
        return std::tie(width, height, refresh_hz) < std::tie(o.width, o.height, o.refresh_hz);
    }
    bool operator==(const Mode &) const = default;
};

struct Display {
    RECT bounds;
    HMONITOR monitor;
    std::vector<Mode> modes;
};

#ifdef _WIN32
// The display as the Windows side sees it: under Wine, the macOS screen.
Display query_display()
{
    Display display{};
    const POINT origin = {0, 0};
    display.monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {sizeof(info)};
    if (GetMonitorInfoW(display.monitor, &info))
        display.bounds = info.rcMonitor;
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
        if (mode.dmBitsPerPel >= 24 && mode.dmPelsWidth && mode.dmPelsHeight)
            display.modes.push_back({mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 60});
    }
    if (display.modes.empty())
        display.modes.push_back({1920, 1080, 60});
    return display;
}
#else
// The headless native build has no screen to ask.
Display query_display()
{
    return {{0, 0, 1920, 1080}, nullptr, {{1280, 720, 60}, {1920, 1080, 60}, {2560, 1440, 60}}};
}
#endif

// The display with its sorted, deduplicated mode list. Applications ask for outputs
// often (on resize, per frame in some engines) and enumerating modes is costly
// under Wine, so the answer is reused for a couple of seconds.
Display current_display()
{
    static std::mutex mutex;
    static Display cached;
    static std::chrono::steady_clock::time_point stamp;
    static bool valid = false;

    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!valid || now - stamp > std::chrono::seconds(2)) {
        cached = query_display();
        std::sort(cached.modes.begin(), cached.modes.end());
        cached.modes.erase(std::unique(cached.modes.begin(), cached.modes.end()), cached.modes.end());
        stamp = now;
        valid = true;
    }
    return cached;
}

bool is_displayable(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

class Output final : public WithPrivateData<RefCounted<IDXGIOutput6>> {
public:
    explicit Output(IDXGIAdapter *parent) : parent_(parent), display_(current_display())
    {
        parent_->AddRef();
    }

    ~Output() override { parent_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, IDXGIObject, IDXGIOutput, IDXGIOutput1, IDXGIOutput2, IDXGIOutput3,
                                IDXGIOutput4, IDXGIOutput5, IDXGIOutput6>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override
    {
        D3D12M_TRACED_BEGIN
        return parent_->QueryInterface(riid, parent);
        D3D12M_TRACED_END(riid, parent)
    }

    // IDXGIOutput
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC *desc) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc)
            return DXGI_ERROR_INVALID_CALL;
        fill_desc(*desc);
        return S_OK;
        D3D12M_TRACED_END(desc)
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT format, UINT, UINT *count, DXGI_MODE_DESC *modes) override
    {
        D3D12M_TRACED_BEGIN
        if (!count)
            return DXGI_ERROR_INVALID_CALL;
        const size_t available = is_displayable(format) ? display_.modes.size() : 0;
        if (!modes) {
            *count = static_cast<UINT>(available);
            return S_OK;
        }
        const UINT capacity = *count;
        *count = static_cast<UINT>(available);
        for (size_t i = 0; i < std::min<size_t>(capacity, available); ++i)
            modes[i] = to_desc(display_.modes[i], format);
        return capacity < available ? DXGI_ERROR_MORE_DATA : S_OK;
        D3D12M_TRACED_END(format, count, modes)
    }

    // The mode of the list with the nearest size to the requested one.
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC *wanted, DXGI_MODE_DESC *closest, IUnknown *) override
    {
        D3D12M_TRACED_BEGIN
        if (!wanted || !closest)
            return DXGI_ERROR_INVALID_CALL;
        const Mode *best = &display_.modes.back();
        long long best_distance = -1;
        for (const Mode &mode : display_.modes) {
            const long long dw = (long long)mode.width - (wanted->Width ? wanted->Width : mode.width);
            const long long dh = (long long)mode.height - (wanted->Height ? wanted->Height : mode.height);
            const long long distance = dw * dw + dh * dh;
            if (best_distance < 0 || distance < best_distance) {
                best = &mode;
                best_distance = distance;
            }
        }
        *closest = to_desc(*best, wanted->Format);
        return S_OK;
        D3D12M_TRACED_END(wanted, closest)
    }

    HRESULT STDMETHODCALLTYPE WaitForVBlank() override
    {
        D3D12M_TRACED_BEGIN
        std::this_thread::sleep_for(std::chrono::microseconds(16667));
        return S_OK;
        D3D12M_TRACED_END()
    }

    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown *, BOOL) override { D3D12M_TRACED_BEGIN return S_OK; D3D12M_TRACED_END() }
    void STDMETHODCALLTYPE ReleaseOwnership() override {}
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *) override { D3D12M_STUB_HR(); }

    // IDXGIOutput1
    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT format, UINT flags, UINT *count, DXGI_MODE_DESC1 *modes) override
    {
        D3D12M_TRACED_BEGIN
        if (!count)
            return DXGI_ERROR_INVALID_CALL;
        if (!modes)
            return GetDisplayModeList(format, flags, count, nullptr);
        // Convert only as many modes as exist: *count is application-controlled.
        UINT available = 0;
        GetDisplayModeList(format, flags, &available, nullptr);
        const UINT capacity = *count;
        std::vector<DXGI_MODE_DESC> plain;
        try {
            plain.resize(std::min(capacity, available));
        } catch (const std::bad_alloc &) {
            return E_OUTOFMEMORY;
        }
        UINT n = static_cast<UINT>(plain.size());
        HRESULT hr = GetDisplayModeList(format, flags, &n, plain.data());
        *count = n;
        for (size_t i = 0; i < plain.size(); ++i) {
            modes[i] = {plain[i].Width, plain[i].Height, plain[i].RefreshRate, plain[i].Format,
                        plain[i].ScanlineOrdering, plain[i].Scaling, FALSE};
        }
        return hr;
        D3D12M_TRACED_END(format, flags, count, modes)
    }

    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1 *wanted, DXGI_MODE_DESC1 *closest, IUnknown *device) override
    {
        D3D12M_TRACED_BEGIN
        if (!wanted || !closest)
            return DXGI_ERROR_INVALID_CALL;
        DXGI_MODE_DESC in = {wanted->Width, wanted->Height, wanted->RefreshRate, wanted->Format, wanted->ScanlineOrdering, wanted->Scaling};
        DXGI_MODE_DESC out;
        if (HRESULT hr = FindClosestMatchingMode(&in, &out, device); FAILED(hr))
            return hr;
        *closest = {out.Width, out.Height, out.RefreshRate, out.Format, out.ScanlineOrdering, out.Scaling, FALSE};
        return S_OK;
        D3D12M_TRACED_END(wanted, closest, device)
    }

    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown *, IDXGIOutputDuplication **) override { D3D12M_STUB_HR(); }

    // IDXGIOutput2..5
    BOOL STDMETHODCALLTYPE SupportsOverlays() override { D3D12M_TRACE(); return FALSE; }

    HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT, IUnknown *, UINT *flags) override
    {
        D3D12M_TRACED_BEGIN
        if (flags)
            *flags = 0;
        return S_OK;
        D3D12M_TRACED_END(flags)
    }

    HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT, DXGI_COLOR_SPACE_TYPE, IUnknown *, UINT *flags) override
    {
        D3D12M_TRACED_BEGIN
        if (flags)
            *flags = 0;
        return S_OK;
        D3D12M_TRACED_END(flags)
    }

    HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown *, UINT, UINT, const DXGI_FORMAT *, IDXGIOutputDuplication **) override
    {
        D3D12M_STUB_HR();
    }

    // IDXGIOutput6: an SDR sRGB display.
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1 *desc) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc)
            return DXGI_ERROR_INVALID_CALL;
        *desc = {};
        DXGI_OUTPUT_DESC base;
        fill_desc(base);
        std::copy(std::begin(base.DeviceName), std::end(base.DeviceName), desc->DeviceName);
        desc->DesktopCoordinates = base.DesktopCoordinates;
        desc->AttachedToDesktop = base.AttachedToDesktop;
        desc->Rotation = base.Rotation;
        desc->Monitor = base.Monitor;
        desc->BitsPerColor = 8;
        desc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        desc->RedPrimary[0] = 0.64f;
        desc->RedPrimary[1] = 0.33f;
        desc->GreenPrimary[0] = 0.30f;
        desc->GreenPrimary[1] = 0.60f;
        desc->BluePrimary[0] = 0.15f;
        desc->BluePrimary[1] = 0.06f;
        desc->WhitePoint[0] = 0.3127f;
        desc->WhitePoint[1] = 0.3290f;
        desc->MinLuminance = 0.5f;
        desc->MaxLuminance = 80.0f;
        desc->MaxFullFrameLuminance = 80.0f;
        return S_OK;
        D3D12M_TRACED_END(desc)
    }

    HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT *flags) override
    {
        D3D12M_TRACED_BEGIN
        if (flags)
            *flags = 0;
        return S_OK;
        D3D12M_TRACED_END(flags)
    }

private:
    void fill_desc(DXGI_OUTPUT_DESC &desc) const
    {
        desc = {};
        static const WCHAR kName[] = {'\\', '\\', '.', '\\', 'D', 'I', 'S', 'P', 'L', 'A', 'Y', '1', 0};
        std::copy(std::begin(kName), std::end(kName), desc.DeviceName);
        desc.DesktopCoordinates = display_.bounds;
        desc.AttachedToDesktop = TRUE;
        desc.Rotation = DXGI_MODE_ROTATION_IDENTITY;
        desc.Monitor = display_.monitor;
    }

    static DXGI_MODE_DESC to_desc(const Mode &mode, DXGI_FORMAT format)
    {
        DXGI_MODE_DESC desc = {};
        desc.Width = mode.width;
        desc.Height = mode.height;
        desc.RefreshRate = {mode.refresh_hz, 1};
        desc.Format = format;
        desc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
        desc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        return desc;
    }

    IDXGIAdapter *parent_;
    Display display_;
};

} // namespace

IDXGIOutput6 *create_output(IDXGIAdapter *parent)
{
    return new Output(parent);
}

} // namespace d3d12m
