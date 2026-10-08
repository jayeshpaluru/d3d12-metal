# Status

Milestone 1 (native arm64 macOS build, headless tests) and milestone 2 (the same
front-end as `d3d12.dll` + `dxgi.dll` under Wine, with swap chains). "Stubbed" methods exist
in the vtable, log `<function> is not implemented` once to stderr and return
`E_NOTIMPL` (or do nothing for `void` methods).

## Exported functions

| Function | State |
|---|---|
| `D3D12CreateDevice` | implemented (always the default Metal device, max feature level 12_0) |
| `D3D12SerializeRootSignature`, `D3D12SerializeVersionedRootSignature` | implemented (RTS0 1.0 and 1.1, valid DXBC checksum) |
| `D3D12CreateRootSignatureDeserializer`, `D3D12CreateVersionedRootSignatureDeserializer` | implemented |
| `D3D12GetDebugInterface` | returns `E_NOINTERFACE` |
| `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2` | implemented |
| `D3D12EnableExperimentalFeatures` (zero features only), `D3D12GetInterface` (`E_NOINTERFACE`), `DXGIGetDebugInterface1` (`E_NOINTERFACE`), `DXGIDeclareAdapterRemovalSupport` | minimal |

## DXGI

| Object | Implemented | Stubbed |
|---|---|---|
| `IDXGIFactory` .. `IDXGIFactory5` | `EnumAdapters`, `EnumAdapters1`, `EnumAdapterByLuid`, `EnumWarpAdapter` (unsupported), `IsCurrent`, `CheckFeatureSupport` (tearing), `MakeWindowAssociation` (recorded), `GetWindowAssociation`, `CreateSwapChain`, `CreateSwapChainForHwnd`, `GetParent`, private data | `CreateSwapChainForCoreWindow`, `CreateSwapChainForComposition`, software adapter, status/occlusion registration, `IDXGIFactory6`/`7` (`EnumAdapterByGpuPreference`) |
| `IDXGIAdapter` .. `IDXGIAdapter3` | `GetDesc`, `GetDesc1`, `GetDesc2`, `CheckInterfaceSupport`, `QueryVideoMemoryInfo`, `SetVideoMemoryReservation`, `EnumOutputs` (one output), private data | budget-change notifications |
| `IDXGIOutput` .. `IDXGIOutput6` (one per adapter) | `GetDesc`, `GetDesc1` (SDR sRGB), `GetDisplayModeList`/`1` (from `EnumDisplaySettings` under Wine, a fixed list natively), `FindClosestMatchingMode`/`1` (nearest size), `WaitForVBlank`, ownership calls, overlay queries (none) | gamma, display surface, duplication, `GetFrameStatistics` |
| `IDXGISwapChain` .. `IDXGISwapChain4` | `Present`, `Present1`, `GetBuffer`, `GetCurrentBackBufferIndex`, `GetDesc`, `GetDesc1`, `GetFullscreenDesc`, `GetHwnd`, `GetDevice` (the queue), `GetParent`, `ResizeBuffers`/`1` (size, format, count, zero size = client area), `ResizeTarget` (accepted), `SetFullscreenState`/`GetFullscreenState` (windowed only: accepted and ignored), `GetContainingOutput`, `GetFrameStatistics` (present count only), `GetLastPresentCount`, `SetMaximumFrameLatency`, `GetMaximumFrameLatency`, `GetFrameLatencyWaitableObject` (real event, Win32 build only), `CheckColorSpaceSupport`/`SetColorSpace1` (sRGB only), background colour and rotation (identity), `GetSourceSize` | `SetSourceSize`, matrix transform, `SetHDRMetaData`, `GetCoreWindow`, HDR colour spaces, stereo, multisampled back buffers |

## D3D12

| Object | Implemented | Stubbed |
|---|---|---|
| `ID3D12Device` .. `ID3D12Device2` (aggregate-returning methods in the explicit Windows form on the Win32 build) | `CreateCommandQueue`, `CreateCommandAllocator`, `CreateGraphicsPipelineState`, `CreateCommandList`, `CreateDescriptorHeap`, `GetDescriptorHandleIncrementSize`, `CreateRootSignature`, `CreateConstantBufferView`, `CreateRenderTargetView`, `CopyDescriptors`, `CopyDescriptorsSimple`, `CreateCommittedResource`, `CreateFence`, `GetCopyableFootprints`, `GetResourceAllocationInfo`, `CheckFeatureSupport` (OPTIONS, OPTIONS1, ARCHITECTURE, ARCHITECTURE1, FEATURE_LEVELS, FORMAT_SUPPORT, MULTISAMPLE_QUALITY_LEVELS, GPU_VIRTUAL_ADDRESS_SUPPORT, SHADER_MODEL, ROOT_SIGNATURE), `GetNodeCount`, `GetAdapterLuid`, `GetDeviceRemovedReason`, `MakeResident`, `Evict`, private data, `SetName` | compute PSOs, heaps, placed/reserved resources, SRV/UAV/DSV/sampler creation, query heaps, command signatures, shared handles, pipeline libraries, `CreatePipelineState`, resource tiling |
| `ID3D12CommandQueue` | `ExecuteCommandLists` (one submit per call), `Signal`, `Wait`, `GetDesc`, `GetTimestampFrequency` | tile mappings, markers, `GetClockCalibration` |
| `ID3D12CommandAllocator` | `Reset` | |
| `ID3D12GraphicsCommandList` / `1` | `Close`, `Reset`, `ClearRenderTargetView`, `OMSetRenderTargets`, `OMSetBlendFactor`, `OMSetStencilRef`, `RSSetViewports`, `RSSetScissorRects`, `IASetPrimitiveTopology`, `IASetVertexBuffers`, `IASetIndexBuffer`, `SetGraphicsRootSignature`, `SetGraphicsRoot32BitConstant(s)`, `SetGraphicsRoot{ConstantBuffer,ShaderResource,UnorderedAccess}View`, `SetGraphicsRootDescriptorTable`, `SetDescriptorHeaps` (no-op), `SetPipelineState`, `DrawInstanced`, `DrawIndexedInstanced`, `ResourceBarrier` (no-op), `CopyBufferRegion`, `CopyResource` (buffers), `CopyTextureRegion` (texture <-> placed buffer footprint) | compute, bundles, depth-stencil views and clears, UAV clears, texture-to-texture copies, resolves, queries, predication, indirect execution, markers, everything else in the vtable |
| `ID3D12Resource` | `Map`, `Unmap`, `GetGPUVirtualAddress`, `GetDesc`, `GetHeapProperties` | `WriteToSubresource`, `ReadFromSubresource` |
| `ID3D12Fence` | `GetCompletedValue`, `SetEventOnCompletion` (null event blocks; a real event is signaled by the device-wide waiter thread), `Signal` | |
| `ID3D12DescriptorHeap` | `GetDesc`, CPU and GPU start handles | |
| `ID3D12PipelineState` | graphics PSOs | `GetCachedBlob` |
| `ID3D12RootSignature` | creation from RTS0 blobs, top-level argument buffer layout | |

## Bridge and backend

- Buffers (all heap types use shared storage), 2D/array/3D textures, descriptor
  heaps, shared events, queues and pipelines (DXIL to Metal through
  libmetalirconverter, vertex fetch, blend, depth-stencil and raster state).
- Command stream: render targets and clears (colour only; the backend builds
  the render passes), pipeline and draw state, vertex and index buffers, root
  argument snapshots, indexed and non-indexed draws, buffer and texture copies.
  Queue signal and wait are bridge calls, not stream records.
- Vertex input through the converter's stage-in function; shader and
  depth-stencil caches per device.
- Every allocation joins a device `MTLResidencySet` attached to each queue.

## Wine build

- Transport, loading, deployment and the PE/unix split: see ARCHITECTURE.md ("Wine build").
- Verified under Wine 11.18 (Gcenx, x86-64, Rosetta 2) on an M2 Max: `wine_basic.exe`
  (device, buffer copies, an offscreen triangle with DXIL shaders, fence events with real
  Win32 events and a second thread), `swapchain_test.exe` (latency object, resize with
  four formats, outputs), `hello_triangle.exe --selftest` (a port of D3D12HelloTriangle with
  DXIL shaders: back buffer centre and corner checked) and `hello_triangle.exe --frames 300`
  (about 115 frames per second at `Present(1, 0)`, one vsync wait per frame is a
  120 Hz panel). `tools/run-wine-tests.sh` runs them all.
- The presented frame is checked through `D3D12METAL_DUMP_PRESENT` (the present pass
  rendered into a readable texture). A real window screenshot (`screencapture -l`) needs an
  awake, unlocked display and Screen Recording permission for the terminal; the test script
  uses it when `tools/find_window.swift` reports the window capturable and otherwise
  reports why it skipped (on the machine this was developed on, the display was asleep
  and the session locked, so no screenshot was taken).
- Shaders must be DXIL: Wine's `d3dcompiler` produces DXBC, which is not supported yet.
- The native `d3d12.dll` override is per prefix or per directory; the layer does not install
  itself into the prefix.

## Known limitations

- Swap chains: windowed only, no HDR (`R16G16B16A16_FLOAT` is presented as sRGB content),
  `SyncInterval` above 1 is treated as 1, no tearing control beyond `SyncInterval` 0,
  multisampled back buffers and `DXGI_SCALING_NONE` are not handled (the image is always
  scaled to the layer). A window resized by the user is stretched until the application
  calls `ResizeBuffers`.
- The frame latency waitable object is an auto-reset event, not a semaphore: a maximum
  frame latency above 1 does not allow more frames in flight.

- Resource barriers are not translated. Metal does not track hazards for
  resources reached only through GPU addresses (CBV tables, root descriptors),
  so a copy that feeds a later draw through such a resource needs explicit
  synchronisation in a later milestone.
- Textures exist only in DEFAULT heaps (private storage). Depth-stencil targets
  cannot be bound yet: the command stream and the replay have no depth plumbing
  (`SET_RENDER_TARGETS` carries colour views only, there is no `CLEAR_DSV`); it
  comes with the DSV milestone, together with a test that exercises it. Until
  then pipelines declare no depth or stencil format and depth-stencil state is
  off (logged once when a PSO asks for it). Draws with no render targets are
  skipped with a one-time message.
- `meson test -C build --setup validation` runs the suite under Metal API
  validation (`MTL_DEBUG_LAYER=1`, `METAL_DEVICE_WRAPPER_TYPE=1`).
- The Win32 build implements the aggregate-returning methods in the explicit form of the MinGW
  headers (`T *Name(T *ret, ...)`), checked only against programs built with the same
  headers. Callers built by MSVC are expected to use the same convention (it is what vkd3d
  implements), but this has not been run against an MSVC-built program.
- `D3D12_FEATURE_LEVEL` is capped at 12_0.

## Known gaps / planned

Deferred on purpose:

- Embedded DXIL root signatures (a PSO with a null `pRootSignature`) are rejected.
- Planar depth-stencil footprints (D24_UNORM_S8_UINT is mapped to a single-plane Depth32Float_Stencil8).
- Resource `Alignment` reporting and the 4 MB alignment of multisampled resources.
- A thread-safety audit of the `MTLResidencySet` handling (the dirty flag and commit are not synchronised with allocation on other threads).
- Per-type descriptor increment sizes (DSV milestone).
- Barrier records and an untracked-hazard mode (SRV/UAV milestone).
- Chunked command-allocator storage.
- A root-argument upload ring.
- Metal 4 argument tables (GPU-address binding).
- An on-disk shader cache / `MTLBinaryArchive`.

Wine notes:

- `FenceWaiter::run` runs on a Windows thread (a `std::thread` built for the PE target is one), never on a Metal callback thread. The listener blocks in `notify.mm` only lock a mutex, queue a record and notify a condition variable.
- `platform_set_event` calls `SetEvent` in `_WIN32` builds; the native build uses the small event object in `src/common/platform.cpp`.
- Every bridge call is a unix call (about a hundred nanoseconds to a few microseconds of overhead, to be measured): per frame the triangle sample makes a handful. A busy game will want `mtlb_format_get_info` and similar pure lookups answered on the PE side, and descriptor writes batched.
