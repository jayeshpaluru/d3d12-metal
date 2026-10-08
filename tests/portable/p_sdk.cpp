// D3D12GetInterface (SDK configuration, DRED settings, unknown classes), D3D12EnableExperimentalFeatures and the
// DXGI video memory budget.
#include <chrono>
#include <thread>

#include "t12.h"

#ifdef _WIN32
// The import library of the MinGW toolchain does not have it (and its headers do not define the class IDs).
HRESULT D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object)
{
    using Fn = HRESULT(WINAPI *)(REFCLSID, REFIID, void **);
    static const Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12GetInterface")));
    CHECK(fn != nullptr);
    return fn(clsid, iid, object);
}
#else
extern "C" HRESULT D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object);
#endif

namespace {
const GUID kSdkConfiguration = {0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};
const GUID kRemovedExtendedData = {0x4a75bbc4, 0x9ff4, 0x4ad8, {0x9f, 0x18, 0xab, 0xae, 0x84, 0xdc, 0x5f, 0xf2}};
}

int main()
{
    // ---- SDK configuration: any SDK version is accepted --------------------------------------------------------
    {
        ComPtr<ID3D12SDKConfiguration> config;
        CHECK_HR(D3D12GetInterface(kSdkConfiguration, IID_PPV_ARGS(config.GetAddressOf())));
        CHECK_HR(config->SetSDKVersion(610, ".\\D3D12\\"));
    }

    // ---- DRED settings ---------------------------------------------------------------------------------------------
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        CHECK_HR(D3D12GetInterface(kRemovedExtendedData, IID_PPV_ARGS(dred.GetAddressOf())));
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }

    // ---- Unknown classes and null output ------------------------------------------------------------------------
    {
        static const GUID unknown = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
        void *object = &object;
        CHECK(D3D12GetInterface(unknown, __uuidof(IUnknown), &object) == E_NOINTERFACE);
        CHECK(object == nullptr);
        CHECK(D3D12GetInterface(kSdkConfiguration, __uuidof(IUnknown), nullptr) == E_INVALIDARG);
    }

    // ---- Experimental features: none can be enabled ----------------------------------------------------------
    {
        CHECK_HR(D3D12EnableExperimentalFeatures(0, nullptr, nullptr, nullptr));
        const IID feature = __uuidof(ID3D12Device);
        CHECK(D3D12EnableExperimentalFeatures(1, &feature, nullptr, nullptr) == E_NOINTERFACE);
    }

    // ---- Video memory budgets ----------------------------------------------------------------------------------
    {
        Gpu gpu;
        ComPtr<IDXGIAdapter1> adapter;
        CHECK_HR(gpu.factory->EnumAdapters1(0, adapter.GetAddressOf()));
        ComPtr<IDXGIAdapter3> adapter3;
        CHECK_HR(adapter->QueryInterface(IID_PPV_ARGS(adapter3.GetAddressOf())));

        DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, non_local = {};
        CHECK_HR(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
        CHECK_HR(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local));
        CHECK(local.Budget >= 256ull * 1024 * 1024);
        CHECK(local.AvailableForReservation > 0);
        CHECK(adapter3->QueryVideoMemoryInfo(1, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local) == E_INVALIDARG);

        // Usage follows allocations (the layer samples it at most every 250 ms, as games poll it every frame).
        DXGI_QUERY_VIDEO_MEMORY_INFO before = {}, after = {};
        CHECK_HR(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &before));
        ComPtr<ID3D12Resource> big = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 256ull * 1024 * 1024);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_HR(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after));
        CHECK(after.CurrentUsage >= before.CurrentUsage + 128ull * 1024 * 1024);

        // Reservations read back.
        CHECK_HR(adapter3->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, 64ull * 1024 * 1024));
        CHECK_HR(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after));
        CHECK(after.CurrentReservation == 64ull * 1024 * 1024);

        // Budget change notifications register and unregister.
        // The layer only records the registration, so any handle value will do.
        HANDLE event = reinterpret_cast<HANDLE>(uintptr_t(1));
        DWORD cookie = 0;
        CHECK_HR(adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(event, &cookie));
        adapter3->UnregisterVideoMemoryBudgetChangeNotification(cookie);
    }

    std::printf("p_sdk: PASS\n");
    return 0;
}
