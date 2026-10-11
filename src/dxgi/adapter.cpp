#include "dxgi/adapter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include "common/com.h"
#include "common/config.h"
#include "common/log.h"
#include "common/luid.h"
#include "common/private_data.h"
#include "dxgi/output.h"

#include "bridge/mtlb.h"

namespace d3d12m {

namespace {

constexpr UINT kVendorIdApple = 0x106B;

// A hexadecimal option ("VENDOR_ID"), or `fallback` when it is not set.
UINT config_hex(const char *key, UINT fallback)
{
    const char *value = config_get(key);
    return value && *value ? static_cast<UINT>(std::strtoul(value, nullptr, 16)) : fallback;
}

// The user-mode driver version CheckInterfaceSupport reports: driver_version=<a.b.c.d> in d3d12metal.conf (the
// DriverVersion string of a Windows driver, which games compare with the one in the registry), else 1.
LONGLONG config_driver_version()
{
    const char *value = config_get("DRIVER_VERSION");
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!value || std::sscanf(value, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 1;
    return static_cast<LONGLONG>((static_cast<uint64_t>(a & 0xffff) << 48) | (static_cast<uint64_t>(b & 0xffff) << 32)
                                 | (static_cast<uint64_t>(c & 0xffff) << 16) | (d & 0xffff));
}

#ifdef _WIN32
// The PCI identity Wine registered for the display adapter (HKLM\\System\\CurrentControlSet\\Enum\\PCI\\VEN_..&DEV_..),
// which is what the adapter's DeviceID string of EnumDisplayDevices names. Games look the driver up through that key
// (Spider-Man reads DriverVersion there and reports "no graphics card" when the key for the ids they were given is missing).
bool query_pci_ids(UINT &vendor, UINT &device, UINT &subsystem, UINT &revision)
{
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, 0, &adapter, 0))
        return false;
    return swscanf(adapter.DeviceID, L"PCI\\VEN_%x&DEV_%x&SUBSYS_%x&REV_%x", &vendor, &device, &subsystem, &revision) == 4;
}

// Creates (volatile) key `path` under `parent` and sets string values {name, value}; a REG_MULTI_SZ for `multi`.
void set_registry_values(HKEY parent, const wchar_t *path, std::initializer_list<std::pair<const wchar_t *, std::wstring>> values,
                         const wchar_t *multi = nullptr)
{
    HKEY key;
    if (RegCreateKeyExW(parent, path, 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        D3D12M_LOG("spoofed adapter: cannot create HKLM\\%ls", path);
        return;
    }
    for (const auto &[name, value] : values) {
        std::wstring data = value;
        DWORD type = REG_SZ;
        if (multi && std::wcscmp(name, multi) == 0) {
            data.push_back(L'\0');  // REG_MULTI_SZ ends with an empty string
            type = REG_MULTI_SZ;
        }
        RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE *>(data.c_str()), static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}

// A spoofed identity (vendor_id / device_id other than the PCI ids Wine registered) also needs the driver keys a
// game finds through SetupAPI: the PCI device (Enum\PCI\VEN_x&DEV_y&...\00000000) and its driver key in the display
// class with DriverVersion (driver_version in the conf). Wine prunes display devices it did not detect itself when
// it refreshes them, so the keys are written by the process, volatile, when its first adapter is made (before the
// game looks: Spider-Man 2 enumerates PCI devices after its first CreateDXGIFactory).
void register_spoofed_driver(const wchar_t *name, UINT vendor, UINT device)
{
    static std::atomic<bool> done{false};
    if (done.exchange(true))
        return;
    const char *version = config_get("DRIVER_VERSION");
    wchar_t ids[64];
    std::swprintf(ids, std::size(ids), L"VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_00", vendor, device);
    const std::wstring hwid = std::wstring(L"PCI\\") + ids;
    const wchar_t *provider = vendor == 0x10de ? L"NVIDIA" : vendor == 0x1002 ? L"Advanced Micro Devices, Inc."
                              : vendor == 0x8086 ? L"Intel Corporation" : L"d3d12-metal";
    constexpr const wchar_t *kDisplayClass = L"{4D36E968-E325-11CE-BFC1-08002BE10318}";
    constexpr const wchar_t *kDriverIndex = L"0100";
    std::wstring version_w;
    for (const char *c = version ? version : "1.0.0.0"; *c; ++c)
        version_w.push_back(static_cast<wchar_t>(*c));
    set_registry_values(HKEY_LOCAL_MACHINE, (std::wstring(L"System\\CurrentControlSet\\Enum\\") + hwid + L"\\00000000").c_str(),
                        {{L"Class", L"Display"},
                         {L"ClassGUID", kDisplayClass},
                         {L"DeviceDesc", name},
                         {L"Mfg", provider},
                         {L"Driver", std::wstring(kDisplayClass) + L"\\" + kDriverIndex},
                         {L"HardwareID", hwid}},
                        L"HardwareID");
    set_registry_values(HKEY_LOCAL_MACHINE,
                        (std::wstring(L"System\\CurrentControlSet\\Control\\Class\\") + kDisplayClass + L"\\" + kDriverIndex).c_str(),
                        {{L"DriverDesc", name},
                         {L"DriverVersion", version_w},
                         {L"ProviderName", provider},
                         {L"MatchingDeviceId", hwid}});
}
#endif

// Copies the members shared by all three adapter description structs.
template <typename Desc>
void fill_common(Desc &d, const DXGI_ADAPTER_DESC2 &src)
{
    std::copy(std::begin(src.Description), std::end(src.Description), d.Description);
    d.VendorId = src.VendorId;
    d.DeviceId = src.DeviceId;
    d.SubSysId = src.SubSysId;
    d.Revision = src.Revision;
    d.DedicatedVideoMemory = src.DedicatedVideoMemory;
    d.DedicatedSystemMemory = src.DedicatedSystemMemory;
    d.SharedSystemMemory = src.SharedSystemMemory;
    d.AdapterLuid = src.AdapterLuid;
}

class Adapter final : public WithPrivateData<RefCounted<IDXGIAdapter3>> {
public:
    // Takes a reference on `parent`.
    Adapter(IDXGIFactory *parent, const mtlb_device_caps &caps) : parent_(parent)
    {
        parent_->AddRef();

        // Adapter descriptions are UTF-16 on Windows; the device name is ASCII.
        const size_t max_chars = std::size(desc_.Description) - 1;
        for (size_t i = 0; i < max_chars && caps.name[i]; i++)
            desc_.Description[i] = static_cast<unsigned char>(caps.name[i]);

        UINT vendor = kVendorIdApple, device = 0, subsystem = 0, revision = 0;
#ifdef _WIN32
        query_pci_ids(vendor, device, subsystem, revision);
#endif
        // adapter_name=<text> in d3d12metal.conf replaces the description (with vendor_id / device_id: an identity for games
        // that whitelist GPUs).
        if (const char *name = config_get("ADAPTER_NAME"); name && *name) {
            std::fill(std::begin(desc_.Description), std::end(desc_.Description), WCHAR(0));
            for (size_t i = 0; i < max_chars && name[i]; i++)
                desc_.Description[i] = static_cast<unsigned char>(name[i]);
        }
        desc_.VendorId = config_hex("VENDOR_ID", vendor);
        desc_.DeviceId = config_hex("DEVICE_ID", device);
#ifdef _WIN32
        if (desc_.VendorId != vendor || desc_.DeviceId != device)
            register_spoofed_driver(desc_.Description, desc_.VendorId, desc_.DeviceId);
#endif
        desc_.SubSysId = subsystem;
        desc_.Revision = revision;
        desc_.DedicatedVideoMemory = caps.recommended_max_working_set_size;
        desc_.SharedSystemMemory = caps.recommended_max_working_set_size;
        desc_.AdapterLuid = luid_from_registry_id(caps.registry_id);
        registry_id_ = caps.registry_id;
    }

    ~Adapter() override { parent_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, IDXGIObject, IDXGIAdapter, IDXGIAdapter1,
                                IDXGIAdapter2, IDXGIAdapter3>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override
    {
        D3D12M_TRACED_BEGIN
        return parent_->QueryInterface(riid, parent);
        D3D12M_TRACED_END(riid, parent)
    }

    // IDXGIAdapter
    // One output, the primary display.
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT index, IDXGIOutput **output) override
    {
        D3D12M_TRACED_BEGIN
        if (!output)
            return DXGI_ERROR_INVALID_CALL;
        *output = nullptr;
        if (index != 0)
            return DXGI_ERROR_NOT_FOUND;
        return hand_out(create_output(this), __uuidof(IDXGIOutput), reinterpret_cast<void **>(output));
        D3D12M_TRACED_END(index, output)
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *desc) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc)
            return E_INVALIDARG;
        fill_common(*desc, desc_);
        return S_OK;
        D3D12M_TRACED_END(desc)
    }

    // The layer is a plain translation layer: every interface "is supported".
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER *umd_version) override
    {
        D3D12M_TRACED_BEGIN
        if (umd_version)
            umd_version->QuadPart = config_driver_version();
        return S_OK;
        D3D12M_TRACED_END(umd_version)
    }

    // IDXGIAdapter1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *desc) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc)
            return E_INVALIDARG;
        fill_common(*desc, desc_);
        desc->Flags = desc_.Flags;
        return S_OK;
        D3D12M_TRACED_END(desc)
    }

    // IDXGIAdapter2
    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *desc) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc)
            return E_INVALIDARG;
        *desc = desc_;
        return S_OK;
        D3D12M_TRACED_END(desc)
    }

    // IDXGIAdapter3
    HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE, DWORD *) override
    {
        D3D12M_STUB_HR();
    }

    void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD) override
    {
        D3D12M_STUB_LOG();
    }

    // Applications such as the target game run their own residency management on this: the budget of the
    // local segment is most of what Metal recommends a process keep resident (the memory is unified), the
    // non-local one a small share, and the usage is what the process has allocated on the device.
    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT node, DXGI_MEMORY_SEGMENT_GROUP group,
                                                   DXGI_QUERY_VIDEO_MEMORY_INFO *info) override
    {
        D3D12M_TRACED_BEGIN
        if (node != 0 || !info || (group != DXGI_MEMORY_SEGMENT_GROUP_LOCAL && group != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL))
            return E_INVALIDARG;
        const bool local = group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
        // Games poll this every frame; the answer (a bridge call) is reused for a quarter of a second.
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t sampled = usage_sampled_ms_.load(std::memory_order_relaxed);
        if (sampled < 0 || now - sampled >= 250) {
            mtlb_device_caps caps;
            usage_.store(mtlb_query_caps(registry_id_, &caps) == MTLB_OK ? caps.current_allocated_size : 0, std::memory_order_relaxed);
            usage_sampled_ms_.store(now, std::memory_order_relaxed);
        }
        const UINT64 used = usage_.load(std::memory_order_relaxed);
        info->Budget = local ? desc_.DedicatedVideoMemory / 10 * 9 : 256ull * 1024 * 1024;
        info->CurrentUsage = local ? used : 0;
        info->AvailableForReservation = info->Budget / 2;
        info->CurrentReservation = reservation_[local ? 0 : 1];
        return S_OK;
        D3D12M_TRACED_END(node, group, info)
    }

    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT node, DXGI_MEMORY_SEGMENT_GROUP group, UINT64 reservation) override
    {
        D3D12M_TRACED_BEGIN
        if (node != 0 || (group != DXGI_MEMORY_SEGMENT_GROUP_LOCAL && group != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL))
            return E_INVALIDARG;
        reservation_[group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL ? 0 : 1] = reservation;
        return S_OK;
        D3D12M_TRACED_END(node, group, reservation)
    }

    // The budget never changes, so the event is never signalled; the cookie only identifies the registration.
    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE event, DWORD *cookie) override
    {
        D3D12M_TRACED_BEGIN
        if (!event || !cookie)
            return E_INVALIDARG;
        *cookie = next_cookie_++;
        return S_OK;
        D3D12M_TRACED_END(event, cookie)
    }

    void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD) override {}

private:
    IDXGIFactory *parent_;
    DXGI_ADAPTER_DESC2 desc_{}; // GetDesc/GetDesc1 are prefixes of this
    uint64_t registry_id_ = 0;
    std::atomic<UINT64> reservation_[2] = {};
    std::atomic<UINT64> usage_{0};            // cached current_allocated_size
    std::atomic<int64_t> usage_sampled_ms_{-1};  // steady-clock milliseconds of the last sample; -1: never
    std::atomic<DWORD> next_cookie_{1};
};

} // namespace

IDXGIAdapter3 *create_adapter(IDXGIFactory *parent, const mtlb_device_caps &caps)
{
    return new Adapter(parent, caps);
}

} // namespace d3d12m
