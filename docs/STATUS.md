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
| `ID3D12GraphicsCommandList` .. `7` | everything for rendering and compute: clears (RTV, DSV, UAV float/uint), `OMSetRenderTargets` with DSV, depth bounds ignored, stencil ref, viewports/scissors, vertex/index buffers, topologies (point, line, line strip, triangle, triangle strip), root arguments for graphics and compute (constants, CBV/SRV/UAV views, tables), `SetDescriptorHeaps`, draws and dispatches, `ExecuteIndirect` (draw, indexed, dispatch, constants, CBV/SRV/UAV views, vertex buffers, count buffer), all copies (buffer, texture, texture to buffer and back, `CopyResource`), `ResolveSubresource`/`ResolveSubresourceRegion` (full-subresource resolves), `ResourceBarrier` (transition, UAV, aliasing; see ARCHITECTURE.md), `BeginQuery`/`EndQuery`/`ResolveQueryData` (occlusion, binary occlusion, timestamp; pipeline and stream-output statistics resolve to zeros), `SetMarker`/`BeginEvent`/`EndEvent`, `WriteBufferImmediate`, `ExecuteBundle`, `BeginRenderPass`/`EndRenderPass` (clear by beginning access, suspend/resume, colour resolve at the end; no depth resolve; tier 0), `OMSetDepthBounds`/`SetPredication` (accepted, no effect) |
| `ID3D12Resource`/`1`/`2` | `Map`, `Unmap`, `GetGPUVirtualAddress`, `GetDesc`/`GetDesc1`, `GetHeapProperties` |
| `ID3D12Heap`/`1` | placed resources, all heap types, `GetDesc` |
| `ID3D12QueryHeap` | occlusion, binary occlusion, timestamp, pipeline statistics, stream-output statistics |
| `ID3D12CommandSignature` | validated at creation; argument types: draw, draw indexed, dispatch, constant, vertex buffer view, CBV, SRV, UAV |
| `ID3D12Fence`/`1` | `GetCompletedValue`, `SetEventOnCompletion` (null event blocks), `Signal` |
| `ID3D12DescriptorHeap`, `ID3D12PipelineState`, `ID3D12RootSignature`, `ID3D12CommandAllocator` | as before (compute PSOs and stream PSOs included) |

Stubbed (log once, `E_NOTIMPL` or no effect): reserved/tiled resources and tile mappings, ray tracing,
mesh/amplification shaders, video, work graphs, pipeline libraries, shared handles, `WriteToSubresource`/`ReadFromSubresource`,
`CopyTiles`, `SOSetTargets` (stream output), `SetSamplePositions`, `AtomicCopyBufferUINT*`, `SetViewInstanceMask`,
variable rate shading, enhanced barriers (`Barrier`), `ClearState`, `SetEventOnMultipleFenceCompletion`,
`SetProtectedResourceSession`, `GetCachedBlob`.

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

## Milestone 4

Descriptor writes without bridge calls, the shader disk cache, thread-safe PSO creation, hardening and the
`D3D12METAL_STATS` counters (ARCHITECTURE.md), and the first real engine: Godot 4.7.2's D3D12 renderer.

- **Godot 4.7.2, Forward+** renders `tests/godot/project` correctly under Wine through these DLLs (`tools/run-godot-test.sh`:
  lit red box and textured sphere with a shadow on a floor, procedural sky, a 2D label; the screenshot is checked for
  colours, sky and floor). 120 frames per second at 640x360 (the panel's refresh rate), 9 unix calls and 26 descriptor
  writes per frame. Mobile: see the open gap above (`SV_ViewID`).
- What Godot asks and what the layer answers: the device is created at feature level 12_0 (Godot accepts 12_0 and
  names it in its log), shader model 6.6, resource binding tier 3, no tiled resources, no ray tracing, mesh
  shaders, variable rate shading or enhanced barriers (Godot then uses legacy barriers), root signature 1.1 and
  no placed-resource or tight-alignment support (the Agility SDK queries 50 to 54 answer "not supported").
  Godot's own DXIL is produced by Mesa's NIR path (no `dxil.dll`, unsigned), which the layer accepts.
- Fixed on the way: pipeline state streams that carry empty graphics subobjects next to a compute shader were refused.
- The Godot console launcher exe does not exit under Wine; the runner uses the GUI exe, which writes stdout as well.
- Run `tools/run-godot-test.sh` after `tools/build-wine.sh`; Godot is not part of the repository (see README).

## Milestone 5: game launch harness

Tooling for getting Marvel's Spider-Man Remastered running; no game code or assets are involved (README, "Running the game").

- **Configuration file.** Every `D3D12METAL_*` option can be set in `d3d12metal.conf` next to `d3d12.dll`; the environment overrides it.
  The PE side reads the file (`src/common/config.cpp`) and sends its entries to the unix side with `mtlb_configure` as the first unix call
  (the unix side `setenv`s them unless the environment already has them), so the backend's own `getenv` options (cache, dump) follow the file.
- **File logging.** `D3D12METAL_LOG_FILE` makes both sides append whole lines to one file. The PE side does not open it: it sends
  its lines through `mtlb_log_write` (one `write(2)` on an `O_APPEND` descriptor in `src/bridge/metal/log.cpp`), so there is no DOS-path
  conversion and lines of the two sides never interleave. Messages before the transport is up (or when it failed to load) go to stderr only.
  Off, the cost is a cached boolean per message.
- **API trace.** `D3D12METAL_TRACE=1`: `D3D12M_TRACE(args...)` / `D3D12M_TRACED_BEGIN ... D3D12M_TRACED_END(args...)` (`src/common/trace.h`)
  are at the top of every D3D12 and DXGI method (HRESULT methods run their body in a lambda so the result can be logged; stubs are
  counted by `D3D12M_STUB_LOG`; `QueryInterface` is traced in `query_interfaces`). Off, each is one predictable branch. A new method needs the macro to
  show up in the inventory. Lines show the arguments (pointers with a summary of common descriptors, IIDs by name) and the HRESULT.
  Calls are logged for the first 50 of each method and every 1000th after that; the method list has exact counts.
- **Scripts.** `tools/install-game.sh`, `tools/uninstall-game.sh`, `tools/run-game.sh` (shared code in `tools/game-common.sh`,
  window listing in `tools/list_windows.swift`).
- **Validated without the game** with `hello_triangle.exe` copied to `Spider-Man.exe` in a fake game folder of the test prefix, run with
  `run-game.sh --direct` and no `WINEDLLOVERRIDES`: the registry overrides load the layer, the conf file is honoured (log file, trace lines,
  method list), screenshots are taken. The real game has not been run yet.

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
  `ResolveQueryData` of timestamps is a CPU copy in the command buffer's completion handler, which the queue's next buffer
  waits for through an event (nothing blocks; a copy or draw in the same list that reads the destination sees stale data).
- Predication, depth bounds, `SetSamplePositions` (the call is
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
- Chunked command-allocator storage, a root-argument upload ring.
- Metal 4 argument tables (GPU-address binding).
- `MTLBinaryArchive` for pipeline state objects (the shader cache is done).
- Real resource state tracking and the enhanced barrier API.
- Render pass API (`BeginRenderPass`), tiled resources, predication, depth bounds, DXR, mesh shaders, VRS.
- Device-removed detection (a failed command buffer is logged, `GetDeviceRemovedReason` stays `S_OK`).

Hardening gaps still open (the milestone 4 work closed the rest, see ARCHITECTURE.md "Milestone 4 design"):

- Occlusion queries keep 8 result slots per query: a query spanning more render passes undercounts.
- Resources referenced only through descriptors (SRV/UAV/CBV tables, vertex and index buffer addresses) are not pinned by
  command lists: the application must keep them alive until the GPU is done, as D3D12 requires. A released buffer
  leaves its address unknown to the backend (the draw or copy is skipped), a released texture behind a descriptor
  is a use after free on the GPU, as on any driver.
- The sampler cache holds at most 4096 distinct samplers (Metal supports about a thousand per device).
- Godot's Mobile renderer does not render: its post-processing shaders use `SV_ViewID` (view instancing), which
  Metal Shader Converter rejects as an unsupported instruction (`dx.op.viewID.i32`), so those pipelines fail.

Wine notes:

- `FenceWaiter::run` runs on a Windows thread (a `std::thread` built for the PE target is one), never on a Metal callback thread. The listener blocks in `notify.mm` only lock a mutex, queue a record and notify a condition variable.
- `platform_set_event` calls `SetEvent` in `_WIN32` builds; the native build uses the small event object in `src/common/platform.cpp`.
- Every bridge call is a unix call: a trivial one (`ID3D12Fence::GetCompletedValue`) costs about 350 ns round trip under Rosetta, measured over 200000 calls. The triangle sample makes a handful per frame. A busy game will want `mtlb_format_get_info` and similar pure lookups answered on the PE side, and descriptor writes batched.

## Spider-Man Remastered (milestone 5, in progress)

- Boots to a window and renders frames (black so far). Findings on the way:
  - The "hang" was a modal `MessageBoxW` ("No installed graphics card has been detected"), invisible while the display slept.
    The game finds the driver through `HKLM\System\CurrentControlSet\Enum\PCI\VEN_x&DEV_x&SUBSYS_x&REV_x` (`DriverVersion`),
    so the adapter now reports the PCI ids Wine registered (`EnumDisplayDevices` DeviceID) instead of device id 0.
    `vendor_id` / `device_id` in d3d12metal.conf override them.
  - The AMD GPU Services library reads dxgi.dll's export table itself; a forwarder RVA crashed it (jump into the string table).
    dxgi.dll now exports real functions (`tests/wine/exports_test.cpp`).
  - crs-video.exe (crash reporter helper) crashes on Wine's `ApiInformation` stub; harmless, ignore.
- Milestone 5b: the game renders its profile-select menu (3D scene and UI), about 60 fps with the API trace on. Fixed on the way:
  - `Fence::GetCompletedValue` reads a shared-memory mirror the backend keeps (updated by command buffer completion of a queue signal
    and by CPU signals), so busy-polling makes no unix call; `GetDeviceRemovedReason` was already front-end only.
  - Failed pipeline creations are cached by description (shader contents, state, root signature) and logged once.
  - DXBC (SM 4/5) shaders: Microsoft's dxilconv builds on macOS (`third_party/dxilconv`, `tools/build-dxilconv.sh`, arm64 and x86_64,
    sources unmodified plus a header shim) and the backend converts DXBC to DXIL before Metal Shader Converter (`src/bridge/metal/dxbc.mm`,
    loaded on first use from `libdxilconv.dylib` next to the module). Test shaders come from Wine's D3DCompile (`tools/gen-dxbc-test-shaders.sh`).
  - Fragment outputs of another type than their render target (uint4 to RGBA8_UNORM and the like) are written through a view of the other kind.
  - Index buffer views of format UNKNOWN unbind; ClearUnorderedAccessViewUint handles R11G11B10_FLOAT; timestamp query heaps are split
    over several counter sample buffers (4096 samples each, made when first used).
  - Open at the time: geometry, hull, domain and stream-output stages were refused (done in milestone 6).

## Milestone 6: gameplay

- **Input without macOS permissions.** `tools/game/keys.c` (built by `tools/game/keys.sh` into `build-wine/out/keys.exe`) runs inside the game's
  prefix and sends keys (`SendInput` with scan codes, or `-m post` for window messages) and mouse clicks to the game window. Menus take keys
  at once; gameplay only reads the keyboard after the window has been clicked. `tools/game/play.sh <name>` launches the game with statistics on
  (the installed `d3d12metal.conf`, `trace=0 stats=0`, stays as it is; the environment overrides it) and drives it from SELECT PROFILE into the
  open world (`tools/game/to-gameplay.sh`: waits for the menu's stats line, profile row 2, CONTINUE, a screenshot every 10 s while loading, a click);
  afterwards `tools/game/keys.sh hold:w:2500 hold:space:300 ...` moves Spider-Man and `tools/game/screenshot.sh <name>` captures.
- **Geometry, hull and domain shaders** run through Metal Shader Converter's mesh shader emulation (`create_emulated_pipeline` in
  `pipeline.mm`, ARCHITECTURE.md "Milestone 6 design"). Stream output is refused with E_NOTIMPL (the converter has no public API for it).
- **Copies between a block-compressed texture and one with a texel per block** (a compressor's R32G32B32A32_UINT output copied into BC6H) go
  through a buffer: Metal refuses a BC view of such a texture, and the failed assertion killed the game on entering the open world.
- Statistics (`D3D12METAL_STATS=1`) now print the frame rate and the GPU time per frame (sum of the command buffers' GPU intervals, and their union).
- **Open world.** Profile 2 loads into the open world (rooftop, museum interior) and renders correctly through the in-game menus; the graphics settings
  menu works (FSR 3.1.4 and XeSS offered, DLSS and frame generation unavailable; applying changes works). Profiling (menu, 120 fps cap, ~6 ms GPU) and gameplay
  (55-60 fps, 12-20 ms of GPU time of which ~8 ms busy, game CPU ~300%) tools: `D3D12METAL_STATS=1`, `D3D12METAL_PASS_PROFILE=1` (GPU time per kind of
  encoder), `D3D12METAL_PROFILE=1` (CPU time per API method, sampled), `D3D12METAL_NO_PASS_MERGE=1`.
- **Pipelines without a root signature** use the one embedded in the shader (XeSS creates its compute pipelines that way).
- **Render passes continue across barriers** that name nothing they render to (`perf` commit); the stats print how many passes only continue the previous one.
- Known open: a `mtlb_queue_signal` access violation once seen right after switching to XeSS in the open world (not reproduced);
  gameplay speed varies with the display state (the stats' GPU time can jump to 160 ms per frame when the display sleeps or another GPU client runs);
  stream output; typed buffer views above 2^28 texels are still clamped.

## Milestone 7: robustness and performance

Code-review fixes (each with a regression test): a render pass continues across a barrier only when none of its draws binds a UAV in any
stage (`pipeline.mm` reads the PSV0 part of the DXIL; no in-pass memory barrier any more); block-size mismatch copies clamp their boxes to the mip
level and stage through pooled hazard-tracked buffers; the fence mirror follows the event's actual value and advances for signals of failed
command buffers (Metal events never go down, so a lower `Signal` value changes nothing; `mtlb_queue_test_drop_signals` simulates a failed buffer);
the typed-output check covers only the targets the pixel shader declares (OSG1); the pipeline failure cache is keyed by root signature content and
skips transient failures; embedded root signatures are shared by content and searched in all stages; a failed counter sample buffer is tried once.

Performance findings (Spider-Man, gameplay scene, `tools/game/measure.sh`):
- The per-method CPU numbers of milestone 6 were mostly the profiler: shared counters bounced between the polling threads and a clock read costs
  hundreds of ns under Wine. The profile now samples per thread. Real costs (`p_descbench` under Wine): `GetCompletedValue` 2.6 ns,
  `GetDeviceRemovedReason` 2.3 ns, `CopyDescriptorsSimple` 95 -> 42 ns per call (the PE build's thread-local storage is emulated: a call per access;
  the heap lookup no longer uses any), 64-descriptor copy 2.1 -> 1.3 ns per descriptor. With the profile on the game ran about 6% slower.
- `D3D12METAL_SUBMIT_PROFILE=1` splits `ExecuteCommandLists` (about 2.6 ms per frame): render encoder creation ~9-11 us each (about 0.9 ms/frame),
  blit ~7 us, compute ~8 us, draws ~0.3-0.6 us, commit ~5 us. Encoder count is the lever. It also lists why passes end: the dominant reason is
  barriers after passes whose draws bind UAVs (about 3000/s). That split is required for correctness on a tile-based GPU (a later vertex stage cannot be
  ordered after an earlier fragment write inside a pass), so it stays.
- Done for the count: a pass that began without waiting waits mid-pass at a barrier instead of ending; a clear of another texture no longer ends the open
  pass (it waits for a pass that binds the view, or for a barrier naming the texture); redundant raster state is not set again.
- `BoundsCheck` off (`D3D12METAL_COMPAT_FLAGS=60`): no measurable GPU change (busy 9.8 vs 9.7 ms), kept on.
- Shader cache: cold run 7792 PSOs in 307 s (all at load), warm run 7720 PSOs in ~7 s with 2688 hits and 0 misses; no PSO creation happens in gameplay (0.00 per frame).
- `D3D12METAL_COMPAT_FLAGS`, `D3D12METAL_GPU_FAMILY` replace the converter settings for experiments (part of the cache key).
- Typed buffer views above 2^28 elements: not solvable here. The limit is the GPU's texture buffer width (measured), Metal Shader Converter has no
  option to lower typed buffers to raw loads (checked `metal_irconverter.h`), and an index cannot be redirected to a second view without rewriting the DXIL.
  Such views stay clamped (`p_large_buffers`).
- XeSS `mtlb_queue_signal` access violation: not reproduced; hardened (the fence is held during Signal/Wait, the queue's destruction waits for in-flight
  submits, pooled buffers return through shared pools instead of a queue pointer in completion handlers).
- Deterministic gameplay entry: `tools/game/to-gameplay.sh` looks at the screen before every key (`tools/game/state.sh`: a capture of the game window
  compared with reference captures in `build-wine/ref/`, made locally with `build-wine/classify --save <png> build-wine/ref/<state>.png` from your own
  screenshots, never committed; states `profile1`, `profile2`, `main-continue`, `gameplay`, else `unknown`). It picks the profile row, confirms only
  CONTINUE, answers a screen that is not the main menu after a profile with Escape (New Game's difficulty menu is never confirmed), and returns when the
  world was seen twice. `measure.sh` aborts unless the world is on screen before and after the sample, takes the CPU from `top` intervals (the old
  `ps %cpu` is a lifetime average and understates), per-thread CPU from `ps -M` time deltas, and leaves the profilers off unless asked.
  The save starts crouched on a roof; a click ends that (Spider-Man walks off), so the click is opt-in (`GAMEPLAY_CLICK=1`).
- **A/B, milestone 6 vs milestone 7, same scene** (profile 2, crouched on the roof, no input, 60 s, no profilers, `GAME_OUT=<build> tools/install-game.sh`):
  m6 460 / 460 / 453 % game CPU (top), 57.4 / 57.9 / 57.2 fps; current 455 / 458 / 454 %, 56.1 / 56.6 / 56.7 fps; GPU busy 7.5-9 ms on both; render passes 92 (m6)
  vs 81-89 (current), barriers ~254 on both. No regression: the 500%+ and the old ~300% are the same load measured differently (lifetime-average `ps`) and
  in different scenes. The CPU is spread over ~35 game threads at 5-20 % each (the game's job system), the layer's submit thread is about 3 ms per frame on both.
  Asynchronous submission (a worker thread for `ExecuteCommandLists`) would take ~3 ms off the submitting thread but needs
  object lifetimes held across it.

## Milestone 8 prep (Spider-Man 2)

- **Game profiles.** `GAME=sm1|sm2` selects exe, Steam app id, install folder, arguments and screen references
  (`build-wine/ref/<game>/`) for install-game.sh, uninstall-game.sh, run-game.sh and tools/game/*; default sm1. launch.sh exports
  `ROSETTA_ADVERTISE_AVX=1`. A missing game folder is reported, not fatal. `SPOOF_GPU=amd tools/install-game.sh` (opt-in) writes
  `adapter_name=` / `vendor_id` / `device_id` into the conf and a matching PCI enum key.
- **DirectStorage GDeflate meta command** (`src/d3d12/meta_command.*`, `src/bridge/metal/gdeflate*`): `EnumerateMetaCommands`,
  `EnumerateMetaCommandParameters`, `CheckFeatureSupport(QUERY_META_COMMAND)` (scratch size), `CreateMetaCommand`,
  `InitializeMetaCommand`, `ExecuteMetaCommand` of command `1bddd090-c47e-459c-8f81-42c9f97a5308`. Two MSL kernels run at the
  command's place in the queue: a one-group prefix sum of tile counts (publishes an indirect 2D grid), then one 32-lane SIMD group per
  64 KiB tile, a port of Microsoft's reference HLSL decoder (THIRD_PARTY_NOTICES.md). Every access is bounds-checked; damaged streams
  and wrong stream ids cannot touch memory outside the buffers. At most 1024 streams per call. `gdeflate=0` (D3D12METAL_GDEFLATE=0)
  hides the command so DirectStorage uses its own shader. `p_gdeflate` (native and Wine): 9 sizes x 6 data kinds x levels 0/1/6/12 against
  the reference compressor, 20 mixed streams, unaligned data, the call/control count rules, 1024 streams, the compute queue, damaged streams.
  Throughput on an M2 Max (`p_gdeflate --bench`, 64 MB of output, 8 decompressions per submit): text-like data at level 1 / 6 3.7 / 6.7
  GB/s, three-symbol data 2.2 / 3.4 GB/s, incompressible (stored blocks) 24-26 GB/s. Not optimised yet: the match copies of a round run one
  after the other with a barrier each; independent copies could run in parallel.
- **Per-shader quirks** (`src/common/quirks.*`): hash = vkd3d-proton's (FNV-1 over the bytecode as passed to the pipeline call), so its
  tables transfer. `quirk_force_compute_barrier=<hex hashes>` in the conf, or the built-in table (Spider-Man2 `0x324071d329f05ccc`, hash
  unverified against our trace). `shader_hashes=1` logs every pipeline's shader hashes. A quirk shader gets a UAV barrier after each
  Dispatch / ExecuteIndirect dispatch. `test_quirks_*`.
- **Feature level.** `feature_level=12_1` reports 12_1 and ROVs. MSC does back ROVs: `p_rov` shows ordered buffer and texture ROVs
  exact (0 of 655360 wrong) where plain UAVs are wrong in about 95% of pixels. Conservative rasterization stays unsupported (Metal has none),
  so this is a test switch, not a conformant 12_1. Default stays 12_0, ROVs off. Fragment buffer 5 (converter uniforms) is now bound.
- **Other fix:** blend state on integer render targets is dropped (Metal asserted; Spider-Man 2 enables it).
- **First boot of Spider-Man 2** (direct launch): DirectStorage creates the GDeflate meta command and initialises it; the game renders its
  first splash ("Sony Interactive Entertainment presents") at ~117 fps with the pipelines it creates, but does not advance past it
  (no ExecuteMetaCommand seen, one compute shader fails DXIL conversion, one typed buffer view format is unsupported).
