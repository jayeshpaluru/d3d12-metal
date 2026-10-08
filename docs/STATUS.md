# Status

Milestone 1 (native arm64 macOS build, headless tests), milestone 2 (the same
front-end as `d3d12.dll` + `dxgi.dll` under Wine, with swap chains) and milestone 3 (the
feature set a real title needs: textures, UAVs and compute, depth/stencil, heaps, barriers,
queues, indirect, MSAA, queries and the rest of the device surface). "Stubbed" methods exist
in the vtable, log `<function> is not implemented` once to stderr and return
`E_NOTIMPL` (or do nothing for `void` methods).

## Exported functions

| Function | State |
|---|---|
| `D3D12CreateDevice` | implemented (always the default Metal device, max feature level 12_0) |
| `D3D12SerializeRootSignature`, `D3D12SerializeVersionedRootSignature` | implemented (RTS0 1.0 and 1.1, valid DXBC checksum) |
| `D3D12CreateRootSignatureDeserializer`, `D3D12CreateVersionedRootSignatureDeserializer` | implemented |
| `D3D12GetDebugInterface` | returns `E_NOINTERFACE` (DRED settings are answered) |
| `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2` | implemented |
| `D3D12GetInterface` | SDK configuration (`ID3D12SDKConfiguration1`: any SDK version accepted and ignored), device factory (`ID3D12DeviceFactory`: `CreateDevice` = `D3D12CreateDevice`), DRED settings and data; any other class `E_NOINTERFACE` |
| `D3D12EnableExperimentalFeatures` (zero features only), `DXGIGetDebugInterface1` (`E_NOINTERFACE`), `DXGIDeclareAdapterRemovalSupport` | minimal |

## DXGI

| Object | Implemented | Stubbed |
|---|---|---|
| `IDXGIFactory` .. `IDXGIFactory5` | `EnumAdapters`, `EnumAdapters1`, `EnumAdapterByLuid`, `EnumWarpAdapter` (unsupported), `IsCurrent`, `CheckFeatureSupport` (tearing), `MakeWindowAssociation` (recorded), `GetWindowAssociation`, `CreateSwapChain`, `CreateSwapChainForHwnd`, `GetParent`, private data | `CreateSwapChainForCoreWindow`, `CreateSwapChainForComposition`, software adapter, status/occlusion registration, `IDXGIFactory6`/`7` (`EnumAdapterByGpuPreference`) |
| `IDXGIAdapter` .. `IDXGIAdapter3` | `GetDesc`, `GetDesc1`, `GetDesc2`, `CheckInterfaceSupport`, `QueryVideoMemoryInfo`, `SetVideoMemoryReservation`, `EnumOutputs` (one output), private data; budgets (90% of the recommended working set local, 256 MB non-local, usage from Metal's allocated size), budget-change registration (cookie only, the event is never signalled) | |
| `IDXGIOutput` .. `IDXGIOutput6` (one per adapter) | `GetDesc`, `GetDesc1` (SDR sRGB), `GetDisplayModeList`/`1` (from `EnumDisplaySettings` under Wine, a fixed list natively), `FindClosestMatchingMode`/`1` (nearest size), `WaitForVBlank`, ownership calls, overlay queries (none) | gamma, display surface, duplication, `GetFrameStatistics` |
| `IDXGISwapChain` .. `IDXGISwapChain4` | `Present`, `Present1`, `GetBuffer`, `GetCurrentBackBufferIndex`, `GetDesc`, `GetDesc1`, `GetFullscreenDesc`, `GetHwnd`, `GetDevice` (the queue), `GetParent`, `ResizeBuffers`/`1` (size, format, count, zero size = client area), `ResizeTarget` (accepted), `SetFullscreenState`/`GetFullscreenState` (windowed only: accepted and ignored), `GetContainingOutput`, `GetFrameStatistics` (present count only), `GetLastPresentCount`, `SetMaximumFrameLatency`, `GetMaximumFrameLatency`, `GetFrameLatencyWaitableObject` (real event, Win32 build only), `CheckColorSpaceSupport`/`SetColorSpace1` (sRGB only), background colour and rotation (identity), `GetSourceSize` | `SetSourceSize`, matrix transform, `SetHDRMetaData`, `GetCoreWindow`, HDR colour spaces, stereo, multisampled back buffers |

## D3D12

Interface versions up to `ID3D12Device10`, `ID3D12GraphicsCommandList7`, `ID3D12Resource2`, `ID3D12Heap1`,
`ID3D12PipelineState`, `ID3D12DeviceRemovedExtendedData2` answer `QueryInterface`; the debug layer interfaces do not. Methods that cannot work (ray tracing,
mesh shaders, video, tiled mapping) return `E_NOTIMPL`/do nothing and log once.

| Object | Implemented |
|---|---|
| `ID3D12Device` .. `Device10` | queues (direct, compute, copy), allocators, lists (`CreateCommandList`/`1`, bundles), graphics PSOs (descs and `CreatePipelineState` streams, including depth-stencil 1/2 and rasterizer 1/2 subobjects), compute PSOs, root signatures (static samplers are emulated), descriptor heaps (CBV/SRV/UAV, sampler, RTV, DSV, per-type increments), `CreateConstantBufferView`, `CreateShaderResourceView` (buffer, typed buffer, texture 1D/2D/3D/cube/arrays/MS, mip and plane ranges, min LOD clamp on `Sample`), `CreateUnorderedAccessView` (buffer, typed buffer, textures; counter resources rejected), `CreateRenderTargetView`, `CreateDepthStencilView` (read-only flags), `CreateSampler`, `CopyDescriptors`/`Simple`, committed resources (`CreateCommittedResource`/`1`/`2`/`3`), heaps (`CreateHeap`/`1`), placed resources (`CreatePlacedResource`/`1`/`2`), `GetResourceAllocationInfo`/`1`/`2`, `GetCopyableFootprints`/`1`, `CreateCommandSignature`, `CreateQueryHeap`, fences, `CheckFeatureSupport` (options 0..22, architecture, feature levels up to 12_0, format support, multisample quality levels, shader model up to 6.6, root signature 1.1, GPU VA, existing heap, serialization, command queue priority, ...), `EnqueueMakeResident`, `SetStablePrivateData`, `GetDeviceRemovedReason`, residency, private data, `SetName` |
| `ID3D12CommandQueue` | `ExecuteCommandLists`, `Signal`, `Wait` (several queues, cross-queue ordering through shared events), `GetDesc`, `GetTimestampFrequency`, `GetClockCalibration`, `SetMarker`/`BeginEvent`/`EndEvent` |
| `ID3D12GraphicsCommandList` .. `7` | everything for rendering and compute: clears (RTV, DSV, UAV float/uint), `OMSetRenderTargets` with DSV, depth bounds ignored, stencil ref, viewports/scissors, vertex/index buffers, topologies (point, line, line strip, triangle, triangle strip), root arguments for graphics and compute (constants, CBV/SRV/UAV views, tables), `SetDescriptorHeaps`, draws and dispatches, `ExecuteIndirect` (draw, indexed, dispatch, constants, CBV/SRV/UAV views, vertex buffers, count buffer), all copies (buffer, texture, texture to buffer and back, `CopyResource`), `ResolveSubresource`/`ResolveSubresourceRegion` (full-subresource resolves), `ResourceBarrier` (transition, UAV, aliasing; see ARCHITECTURE.md), `BeginQuery`/`EndQuery`/`ResolveQueryData` (occlusion, binary occlusion, timestamp; pipeline and stream-output statistics resolve to zeros), `SetMarker`/`BeginEvent`/`EndEvent`, `WriteBufferImmediate`, `ExecuteBundle`, `OMSetDepthBounds`/`SetPredication` (accepted, no effect) |
| `ID3D12Resource`/`1`/`2` | `Map`, `Unmap`, `GetGPUVirtualAddress`, `GetDesc`/`GetDesc1`, `GetHeapProperties` |
| `ID3D12Heap`/`1` | placed resources, all heap types, `GetDesc` |
| `ID3D12QueryHeap` | occlusion, binary occlusion, timestamp, pipeline statistics, stream-output statistics |
| `ID3D12CommandSignature` | validated at creation; argument types: draw, draw indexed, dispatch, constant, vertex buffer view, CBV, SRV, UAV |
| `ID3D12Fence`/`1` | `GetCompletedValue`, `SetEventOnCompletion` (null event blocks), `Signal` |
| `ID3D12DescriptorHeap`, `ID3D12PipelineState`, `ID3D12RootSignature`, `ID3D12CommandAllocator` | as before (compute PSOs and stream PSOs included) |

Stubbed (log once, `E_NOTIMPL` or no effect): reserved/tiled resources and tile mappings, ray tracing,
mesh/amplification shaders, video, work graphs, pipeline libraries, shared handles, `WriteToSubresource`/`ReadFromSubresource`,
`CopyTiles`, `SOSetTargets` (stream output), `SetSamplePositions`, `AtomicCopyBufferUINT*`, `SetViewInstanceMask`,
variable rate shading, enhanced barriers (`Barrier`), `ClearState`, `SetEventOnMultipleFenceCompletion`, render passes
(`BeginRenderPass`/`EndRenderPass`), `SetProtectedResourceSession`, `GetCachedBlob`.

## Bridge and backend

- Buffers (all heap types use shared storage), textures of every dimension (private storage
  in DEFAULT heaps, shared elsewhere), typeless formats with per-view reinterpretation,
  descriptor heaps (24-byte entries for the converter's tables), samplers, heaps with
  placed resources, query heaps, shared events, queues and pipelines (DXIL to Metal through
  libmetalirconverter, vertex fetch, blend, depth-stencil and raster state, compute).
- Command stream: render targets with depth-stencil, clears, pipeline and draw state, root
  argument snapshots for graphics and compute, descriptor heap binding, draws and dispatches,
  buffer and texture copies, UAV clears, barriers, indirect execution, resolves, queries,
  markers and immediate writes. Queue signal and wait are bridge calls, not stream records.
- Internal compute kernels (MSL compiled at runtime): buffer clears, texture clears, indirect
  argument translation, immediate writes.
- Every allocation joins a device `MTLResidencySet` attached to each queue.

## Wine build

- Transport, loading, deployment and the PE/unix split: see ARCHITECTURE.md ("Wine build").
- Verified under Wine 11.18 (Gcenx, x86-64, Rosetta 2) on an M2 Max: `wine_basic.exe`
  (device, buffer copies, an offscreen triangle with DXIL shaders, fence events with real
  Win32 events and a second thread), `swapchain_test.exe` (latency object, resize with
  four formats, outputs), `hello_triangle.exe --selftest` (a port of D3D12HelloTriangle with
  DXIL shaders: back buffer centre and corner checked) and `hello_triangle.exe --frames 300`
  (about 115 frames per second at `Present(1, 0)` with a wait for the GPU after every
  frame, as the sample does; the panel is 120 Hz). `tools/run-wine-tests.sh` runs them all.
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

- Resource states are not tracked: a barrier is a synchronisation point whatever its states
  (see ARCHITECTURE.md), and nothing is validated.
- D24_UNORM_S8_UINT is Depth32Float_Stencil8 and D24_UNORM_X8 is Depth32Float; there are no
  planar footprints for them.
- Typed buffer views are limited to 2^28 texels (Metal's texture-buffer limit). The converter
  ignores the padding in a typed buffer descriptor: the first element of a view should be
  16-byte aligned.
- A minimum LOD clamp in a sampler is honoured only by `Sample` (the converter's restriction);
  `SampleLevel` and `SampleGrad` ignore it.
- Static samplers are emulated with an extra descriptor table (Metal Shader Converter has no
  static samplers). Metal has three border colours; others are mapped to the nearest.
- `ResolveSubresourceRegion` resolves whole subresources; rectangles are ignored.
- Only one occlusion query is active at a time (D3D12 allows nesting only for different
  types); timestamps are sampled at the boundary of a compute pass of their own, and
  `ResolveQueryData` of timestamps finishes the command buffer so far (a CPU wait during
  the submit). Pipeline-statistics and stream-output queries resolve to zeros.
- Predication, depth bounds, `SetSamplePositions`, adjacency and patch topologies (the call is
  ignored and logged) are not supported.
- Reserved (tiled) resources are refused; shared handles, pipeline libraries, ray tracing,
  mesh shaders and variable rate shading are absent (the matching features report unsupported).
- Command signatures with an index-buffer-view, ray-tracing or mesh argument are refused.
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
- A thread-safety audit of the `MTLResidencySet` handling (the dirty flag and commit are not synchronised with allocation on other threads).
- Chunked command-allocator storage, a root-argument upload ring.
- Metal 4 argument tables (GPU-address binding).
- An on-disk shader cache / `MTLBinaryArchive`.
- Real resource state tracking and the enhanced barrier API.
- Render pass API (`BeginRenderPass`), tiled resources, predication, depth bounds, DXR, mesh shaders, VRS.
- Device-removed detection (a failed command buffer is logged, `GetDeviceRemovedReason` stays `S_OK`).

Wine notes:

- `FenceWaiter::run` runs on a Windows thread (a `std::thread` built for the PE target is one), never on a Metal callback thread. The listener blocks in `notify.mm` only lock a mutex, queue a record and notify a condition variable.
- `platform_set_event` calls `SetEvent` in `_WIN32` builds; the native build uses the small event object in `src/common/platform.cpp`.
- Every bridge call is a unix call: a trivial one (`ID3D12Fence::GetCompletedValue`) costs about 350 ns round trip under Rosetta, measured over 200000 calls. The triangle sample makes a handful per frame. A busy game will want `mtlb_format_get_info` and similar pure lookups answered on the PE side, and descriptor writes batched.
