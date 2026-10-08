// Declarations of the DXGI interfaces and types the layer implements.
//
// DirectX-Headers ships d3d12.h and dxgiformat.h/dxgicommon.h but not dxgi.h
// or dxgi1_x.h, so the subset needed here is declared by hand. Vtable order
// and IIDs match the Windows SDK.
#pragma once

#ifdef _WIN32
// The MinGW headers declare all of DXGI, with the Windows ABI.
#include <windows.h>
#include <dxgi1_6.h>
#else

// Must come before the D3D headers: it defines IUnknown for the wsl stubs.
#include <unknwn.h>

#include <directx/dxgicommon.h>
#include <directx/dxgiformat.h>

typedef void *HMODULE;
typedef void *HMONITOR;

interface IDXGIOutput;
interface IDXGISwapChain;
interface IDXGISwapChain1;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef UINT DXGI_USAGE;

typedef enum DXGI_ADAPTER_FLAG {
    DXGI_ADAPTER_FLAG_NONE = 0,
    DXGI_ADAPTER_FLAG_REMOTE = 1,
    DXGI_ADAPTER_FLAG_SOFTWARE = 2,
    DXGI_ADAPTER_FLAG_FORCE_DWORD = 0xffffffff
} DXGI_ADAPTER_FLAG;

typedef enum DXGI_FEATURE {
    DXGI_FEATURE_PRESENT_ALLOW_TEARING = 0
} DXGI_FEATURE;

typedef enum DXGI_MEMORY_SEGMENT_GROUP {
    DXGI_MEMORY_SEGMENT_GROUP_LOCAL = 0,
    DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1
} DXGI_MEMORY_SEGMENT_GROUP;

typedef enum DXGI_GRAPHICS_PREEMPTION_GRANULARITY {
    DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY = 0,
    DXGI_GRAPHICS_PREEMPTION_PRIMITIVE_BOUNDARY = 1,
    DXGI_GRAPHICS_PREEMPTION_TRIANGLE_BOUNDARY = 2,
    DXGI_GRAPHICS_PREEMPTION_PIXEL_BOUNDARY = 3,
    DXGI_GRAPHICS_PREEMPTION_INSTRUCTION_BOUNDARY = 4
} DXGI_GRAPHICS_PREEMPTION_GRANULARITY;

typedef enum DXGI_COMPUTE_PREEMPTION_GRANULARITY {
    DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY = 0,
    DXGI_COMPUTE_PREEMPTION_DISPATCH_BOUNDARY = 1,
    DXGI_COMPUTE_PREEMPTION_THREAD_GROUP_BOUNDARY = 2,
    DXGI_COMPUTE_PREEMPTION_THREAD_BOUNDARY = 3,
    DXGI_COMPUTE_PREEMPTION_INSTRUCTION_BOUNDARY = 4
} DXGI_COMPUTE_PREEMPTION_GRANULARITY;

typedef enum DXGI_MODE_SCANLINE_ORDER {
    DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED = 0,
    DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE = 1,
    DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST = 2,
    DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST = 3
} DXGI_MODE_SCANLINE_ORDER;

typedef enum DXGI_MODE_SCALING {
    DXGI_MODE_SCALING_UNSPECIFIED = 0,
    DXGI_MODE_SCALING_CENTERED = 1,
    DXGI_MODE_SCALING_STRETCHED = 2
} DXGI_MODE_SCALING;

typedef enum DXGI_SCALING {
    DXGI_SCALING_STRETCH = 0,
    DXGI_SCALING_NONE = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2
} DXGI_SCALING;

typedef enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4
} DXGI_SWAP_EFFECT;

typedef enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3,
    DXGI_ALPHA_MODE_FORCE_DWORD = 0xffffffff
} DXGI_ALPHA_MODE;

typedef struct DXGI_MODE_DESC {
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
} DXGI_MODE_DESC;

typedef struct DXGI_SWAP_CHAIN_DESC {
    DXGI_MODE_DESC BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    BOOL Windowed;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT Flags;
} DXGI_SWAP_CHAIN_DESC;

typedef struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
} DXGI_SWAP_CHAIN_DESC1;

typedef struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC {
    DXGI_RATIONAL RefreshRate;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
    BOOL Windowed;
} DXGI_SWAP_CHAIN_FULLSCREEN_DESC;

typedef struct DXGI_ADAPTER_DESC {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
} DXGI_ADAPTER_DESC;

typedef struct DXGI_ADAPTER_DESC1 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
    UINT Flags;
} DXGI_ADAPTER_DESC1;

typedef struct DXGI_ADAPTER_DESC2 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
    UINT Flags;
    DXGI_GRAPHICS_PREEMPTION_GRANULARITY GraphicsPreemptionGranularity;
    DXGI_COMPUTE_PREEMPTION_GRANULARITY ComputePreemptionGranularity;
} DXGI_ADAPTER_DESC2;

typedef struct DXGI_QUERY_VIDEO_MEMORY_INFO {
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 AvailableForReservation;
    UINT64 CurrentReservation;
} DXGI_QUERY_VIDEO_MEMORY_INFO;

// ---------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------

MIDL_INTERFACE("aec22fb8-76f3-4639-9be0-28eb43a67a2e")
IDXGIObject : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) = 0;
};

MIDL_INTERFACE("3d3e0379-f9de-4d58-bb6c-18d62992f1a6")
IDXGIDeviceSubObject : public IDXGIObject
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) = 0;
};

MIDL_INTERFACE("2411e7e1-12ac-4ccf-bd14-9798e8534dc0")
IDXGIAdapter : public IDXGIObject
{
public:
    virtual HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput **ppOutput) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER *pUMDVersion) = 0;
};

MIDL_INTERFACE("29038f61-3839-4626-91fd-086879011a05")
IDXGIAdapter1 : public IDXGIAdapter
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *pDesc) = 0;
};

MIDL_INTERFACE("0aa1ae0a-fa0e-4b84-8644-e05ff8e5acb5")
IDXGIAdapter2 : public IDXGIAdapter1
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *pDesc) = 0;
};

MIDL_INTERFACE("645967a4-1392-4310-a798-8053ce3e93fd")
IDXGIAdapter3 : public IDXGIAdapter2
{
public:
    virtual HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE hEvent, DWORD *pdwCookie) = 0;
    virtual void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, DXGI_QUERY_VIDEO_MEMORY_INFO *pVideoMemoryInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, UINT64 Reservation) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE hEvent, DWORD *pdwCookie) = 0;
    virtual void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie) = 0;
};

MIDL_INTERFACE("7b7166ec-21c7-44ae-b21a-c9ae321ae369")
IDXGIFactory : public IDXGIObject
{
public:
    virtual HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter **ppAdapter) = 0;
    virtual HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) = 0;
};

MIDL_INTERFACE("770aae78-f26f-4dba-a829-253c83d1b387")
IDXGIFactory1 : public IDXGIFactory
{
public:
    virtual HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) = 0;
    virtual BOOL STDMETHODCALLTYPE IsCurrent() = 0;
};

MIDL_INTERFACE("50c83a1c-e072-4c48-87b0-3630fa36a6d0")
IDXGIFactory2 : public IDXGIFactory1
{
public:
    virtual BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID *pLuid) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD *pdwCookie) = 0;
    virtual void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) = 0;
    virtual void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) = 0;
};

MIDL_INTERFACE("25483823-cd46-4c7d-86ca-47aa95b837bd")
IDXGIFactory3 : public IDXGIFactory2
{
public:
    virtual UINT STDMETHODCALLTYPE GetCreationFlags() = 0;
};

MIDL_INTERFACE("1bc6ea02-ef36-464f-bf0c-21ca39e5168a")
IDXGIFactory4 : public IDXGIFactory3
{
public:
    virtual HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void **ppvAdapter) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void **ppvAdapter) = 0;
};

MIDL_INTERFACE("7632e1f5-ee65-4dca-87fd-84cd75f8838d")
IDXGIFactory5 : public IDXGIFactory4
{
public:
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) = 0;
};

// ---------------------------------------------------------------------------
// Swap chains and outputs
// ---------------------------------------------------------------------------

#define DXGI_USAGE_SHADER_INPUT 0x10UL
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x20UL
#define DXGI_USAGE_BACK_BUFFER 0x40UL
#define DXGI_USAGE_UNORDERED_ACCESS 0x80UL

#define DXGI_PRESENT_TEST 0x00000001UL
#define DXGI_PRESENT_DO_NOT_SEQUENCE 0x00000002UL
#define DXGI_PRESENT_RESTART 0x00000004UL
#define DXGI_PRESENT_DO_NOT_WAIT 0x00000008UL
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL

#define DXGI_MWA_NO_WINDOW_CHANGES (1 << 0)
#define DXGI_MWA_NO_ALT_ENTER (1 << 1)
#define DXGI_MWA_NO_PRINT_SCREEN (1 << 2)

#define DXGI_ENUM_MODES_INTERLACED 1UL
#define DXGI_ENUM_MODES_SCALING 2UL

#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0001L)
#define DXGI_ERROR_INVALID_CALL ((HRESULT)0x887A0001L)
#define DXGI_ERROR_WAS_STILL_DRAWING ((HRESULT)0x887A000AL)

typedef enum DXGI_SWAP_CHAIN_FLAG {
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE = 4,
    DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT = 8,
    DXGI_SWAP_CHAIN_FLAG_RESTRICT_SHARED_RESOURCE_DRIVER = 16,
    DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY = 32,
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 64,
    DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER = 128,
    DXGI_SWAP_CHAIN_FLAG_FULLSCREEN_VIDEO = 256,
    DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO = 512,
    DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED = 1024,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048,
    DXGI_SWAP_CHAIN_FLAG_RESTRICTED_TO_ALL_HOLOGRAPHIC_DISPLAYS = 4096
} DXGI_SWAP_CHAIN_FLAG;

typedef enum DXGI_MODE_ROTATION {
    DXGI_MODE_ROTATION_UNSPECIFIED = 0,
    DXGI_MODE_ROTATION_IDENTITY = 1,
    DXGI_MODE_ROTATION_ROTATE90 = 2,
    DXGI_MODE_ROTATION_ROTATE180 = 3,
    DXGI_MODE_ROTATION_ROTATE270 = 4
} DXGI_MODE_ROTATION;

typedef enum DXGI_HDR_METADATA_TYPE {
    DXGI_HDR_METADATA_TYPE_NONE = 0,
    DXGI_HDR_METADATA_TYPE_HDR10 = 1,
    DXGI_HDR_METADATA_TYPE_HDR10PLUS = 2
} DXGI_HDR_METADATA_TYPE;

typedef enum DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG {
    DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT = 1,
    DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT = 2
} DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG;

typedef struct DXGI_RGBA {
    FLOAT r, g, b, a;
} DXGI_RGBA;

typedef struct DXGI_MATRIX_3X2_F {
    FLOAT _11, _12, _21, _22, _31, _32;
} DXGI_MATRIX_3X2_F;

typedef struct DXGI_MODE_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
    BOOL Stereo;
} DXGI_MODE_DESC1;

typedef struct DXGI_FRAME_STATISTICS {
    UINT PresentCount;
    UINT PresentRefreshCount;
    UINT SyncRefreshCount;
    LARGE_INTEGER SyncQPCTime;
    LARGE_INTEGER SyncGPUTime;
} DXGI_FRAME_STATISTICS;

typedef struct DXGI_PRESENT_PARAMETERS {
    UINT DirtyRectsCount;
    RECT *pDirtyRects;
    RECT *pScrollRect;
    POINT *pScrollOffset;
} DXGI_PRESENT_PARAMETERS;

typedef struct DXGI_GAMMA_CONTROL_CAPABILITIES DXGI_GAMMA_CONTROL_CAPABILITIES;
typedef struct DXGI_GAMMA_CONTROL DXGI_GAMMA_CONTROL;

typedef struct DXGI_OUTPUT_DESC {
    WCHAR DeviceName[32];
    RECT DesktopCoordinates;
    BOOL AttachedToDesktop;
    DXGI_MODE_ROTATION Rotation;
    HMONITOR Monitor;
} DXGI_OUTPUT_DESC;

typedef struct DXGI_OUTPUT_DESC1 {
    WCHAR DeviceName[32];
    RECT DesktopCoordinates;
    BOOL AttachedToDesktop;
    DXGI_MODE_ROTATION Rotation;
    HMONITOR Monitor;
    UINT BitsPerColor;
    DXGI_COLOR_SPACE_TYPE ColorSpace;
    FLOAT RedPrimary[2];
    FLOAT GreenPrimary[2];
    FLOAT BluePrimary[2];
    FLOAT WhitePoint[2];
    FLOAT MinLuminance;
    FLOAT MaxLuminance;
    FLOAT MaxFullFrameLuminance;
} DXGI_OUTPUT_DESC1;

interface IDXGISurface;
interface IDXGIResource;
interface IDXGIOutputDuplication;

MIDL_INTERFACE("ae02eedb-c735-4690-8d52-5a8dc20213aa")
IDXGIOutput : public IDXGIObject
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, UINT *pNumModes, DXGI_MODE_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC *pModeToMatch, DXGI_MODE_DESC *pClosestMatch, IUnknown *pConcernedDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForVBlank() = 0;
    virtual HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown *pDevice, BOOL Exclusive) = 0;
    virtual void STDMETHODCALLTYPE ReleaseOwnership() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES *pGammaCaps) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL *pArray) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL *pArray) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface *pScanoutSurface) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface *pDestination) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) = 0;
};

MIDL_INTERFACE("00cddea8-939b-4b83-a340-a685226666cc")
IDXGIOutput1 : public IDXGIOutput
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT EnumFormat, UINT Flags, UINT *pNumModes, DXGI_MODE_DESC1 *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1 *pModeToMatch, DXGI_MODE_DESC1 *pClosestMatch, IUnknown *pConcernedDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource *pDestination) = 0;
    virtual HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown *pDevice, IDXGIOutputDuplication **ppOutputDuplication) = 0;
};

MIDL_INTERFACE("595e39d1-2724-4663-99b1-da969de28364")
IDXGIOutput2 : public IDXGIOutput1
{
public:
    virtual BOOL STDMETHODCALLTYPE SupportsOverlays() = 0;
};

MIDL_INTERFACE("8a6bb301-7e7e-41f4-a8e0-5b32f7f99b18")
IDXGIOutput3 : public IDXGIOutput2
{
public:
    virtual HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT EnumFormat, IUnknown *pConcernedDevice, UINT *pFlags) = 0;
};

MIDL_INTERFACE("dc7dca35-2196-414d-9f53-617884032a60")
IDXGIOutput4 : public IDXGIOutput3
{
public:
    virtual HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT Format, DXGI_COLOR_SPACE_TYPE ColorSpace, IUnknown *pConcernedDevice, UINT *pFlags) = 0;
};

MIDL_INTERFACE("80a07424-ab52-42eb-833c-0c42fd282d98")
IDXGIOutput5 : public IDXGIOutput4
{
public:
    virtual HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown *pDevice, UINT Flags, UINT SupportedFormatsCount, const DXGI_FORMAT *pSupportedFormats, IDXGIOutputDuplication **ppOutputDuplication) = 0;
};

MIDL_INTERFACE("068346e8-aaec-4b84-add7-137f513f77a1")
IDXGIOutput6 : public IDXGIOutput5
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1 *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT *pFlags) = 0;
};

MIDL_INTERFACE("310d36a0-d2e7-4c0a-aa04-6a9d23b8886a")
IDXGISwapChain : public IDXGIDeviceSubObject
{
public:
    virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) = 0;
};

MIDL_INTERFACE("790a45f7-0d42-4876-983a-0a55cfe6f4aa")
IDXGISwapChain1 : public IDXGISwapChain
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) = 0;
    virtual HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters) = 0;
    virtual BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) = 0;
};

MIDL_INTERFACE("a8be2ac4-199f-4946-b331-79599fb98de7")
IDXGISwapChain2 : public IDXGISwapChain1
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) = 0;
    virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) = 0;
};

MIDL_INTERFACE("94d99bdb-f1f8-4ab0-b236-7da0170edab1")
IDXGISwapChain3 : public IDXGISwapChain2
{
public:
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT *pCreationNodeMask, IUnknown *const *ppPresentQueue) = 0;
};

MIDL_INTERFACE("3d585d5a-bd4a-489e-b1f4-3dbcb6452ffb")
IDXGISwapChain4 : public IDXGISwapChain3
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) = 0;
};

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IDXGIObject, 0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e)
__CRT_UUID_DECL(IDXGIDeviceSubObject, 0x3d3e0379, 0xf9de, 0x4d58, 0xbb, 0x6c, 0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6)
__CRT_UUID_DECL(IDXGIAdapter, 0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0)
__CRT_UUID_DECL(IDXGIAdapter1, 0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05)
__CRT_UUID_DECL(IDXGIAdapter2, 0x0aa1ae0a, 0xfa0e, 0x4b84, 0x86, 0x44, 0xe0, 0x5f, 0xf8, 0xe5, 0xac, 0xb5)
__CRT_UUID_DECL(IDXGIAdapter3, 0x645967a4, 0x1392, 0x4310, 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd)
__CRT_UUID_DECL(IDXGIFactory, 0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69)
__CRT_UUID_DECL(IDXGIFactory1, 0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87)
__CRT_UUID_DECL(IDXGIFactory2, 0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0)
__CRT_UUID_DECL(IDXGIFactory3, 0x25483823, 0xcd46, 0x4c7d, 0x86, 0xca, 0x47, 0xaa, 0x95, 0xb8, 0x37, 0xbd)
__CRT_UUID_DECL(IDXGIFactory4, 0x1bc6ea02, 0xef36, 0x464f, 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a)
__CRT_UUID_DECL(IDXGIFactory5, 0x7632e1f5, 0xee65, 0x4dca, 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d)
__CRT_UUID_DECL(IDXGIOutput, 0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa)
__CRT_UUID_DECL(IDXGIOutput1, 0x00cddea8, 0x939b, 0x4b83, 0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66, 0xcc)
__CRT_UUID_DECL(IDXGIOutput2, 0x595e39d1, 0x2724, 0x4663, 0x99, 0xb1, 0xda, 0x96, 0x9d, 0xe2, 0x83, 0x64)
__CRT_UUID_DECL(IDXGIOutput3, 0x8a6bb301, 0x7e7e, 0x41f4, 0xa8, 0xe0, 0x5b, 0x32, 0xf7, 0xf9, 0x9b, 0x18)
__CRT_UUID_DECL(IDXGIOutput4, 0xdc7dca35, 0x2196, 0x414d, 0x9f, 0x53, 0x61, 0x78, 0x84, 0x03, 0x2a, 0x60)
__CRT_UUID_DECL(IDXGIOutput5, 0x80a07424, 0xab52, 0x42eb, 0x83, 0x3c, 0x0c, 0x42, 0xfd, 0x28, 0x2d, 0x98)
__CRT_UUID_DECL(IDXGIOutput6, 0x068346e8, 0xaaec, 0x4b84, 0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1)
__CRT_UUID_DECL(IDXGISwapChain, 0x310d36a0, 0xd2e7, 0x4c0a, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a)
__CRT_UUID_DECL(IDXGISwapChain1, 0x790a45f7, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa)
__CRT_UUID_DECL(IDXGISwapChain2, 0xa8be2ac4, 0x199f, 0x4946, 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7)
__CRT_UUID_DECL(IDXGISwapChain3, 0x94d99bdb, 0xf1f8, 0x4ab0, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1)
__CRT_UUID_DECL(IDXGISwapChain4, 0x3d585d5a, 0xbd4a, 0x489e, 0xb1, 0xf4, 0x3d, 0xbc, 0xb6, 0x45, 0x2f, 0xfb)
#endif

// dxgi.dll entry points.
extern "C" {
HRESULT CreateDXGIFactory(REFIID riid, void **ppFactory);
HRESULT CreateDXGIFactory1(REFIID riid, void **ppFactory);
HRESULT CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory);
}

#endif // _WIN32
