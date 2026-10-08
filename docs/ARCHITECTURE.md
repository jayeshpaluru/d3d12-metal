# Architecture

```
 Game (x86-64 PE, under Wine + Rosetta)
   │  D3D12 / DXGI COM calls
   ▼
 d3d12.dll / dxgi.dll  ── "front-end": portable C++ (src/d3d12, src/dxgi)
   │  object create/destroy + one submit per ExecuteCommandLists (one span per list)
   ▼
 mtlb bridge C API (src/bridge/mtlb.h)  ── POD structs + opaque handles only
   │  native build: direct call    │  Wine build: unix call (generated from mtlb.h)
   ▼                               ▼
 Metal backend (src/bridge/metal/*.mm) ── Objective-C++, Metal, QuartzCore,
                                          libmetalirconverter
```

## Principles

- **The front-end never touches Metal.** It only calls `mtlb_*`. That lets the
  same front-end compile natively on arm64 macOS (fast headless tests) and as
  x86-64 PE DLLs for Wine.
- **Few bridge crossings.** Command lists are recorded on the front-end side into
  a flat, POD command stream (`src/bridge/mtlb_cmd.h`). `ExecuteCommandLists`
  hands the backend one `mtlb_span` per list in a single `mtlb_queue_submit`
  call (no concatenation copy); each list's stream starts with `RESET_STATE`, so
  the backend replays all spans into the same `MTLCommandBuffer`. Under Wine
  that means one unix call per submit. Root arguments are replayed straight from
  the bytes in the stream.
- **The backend owns render passes.** Records are D3D12-shaped
  (`SET_RENDER_TARGETS`, `CLEAR_RTV`, `DRAW*`, copies). The front-end keeps no
  pass state. The backend `Replay` tracks pending clears per render target view,
  opens the render encoder lazily at the first draw (clears of bound views
  become load actions), keeps it open when the same targets are set again, and
  closes it when the targets change, a clear hits a bound view after draws, a
  copy arrives or the submit ends. Clears that never meet a draw run as
  clear-only passes. `mtlb_queue_render_pass_count` exposes the number of
  encoders for tests.
- **One queue synchronisation mechanism.** Submits, `Signal` and `Wait` all
  encode into the queue's open `MTLCommandBuffer` (retaining its references:
  applications release objects as soon as a fence is signalled, possibly before
  the buffer retires, and `commandBufferWithUnretainedReferences` trips Metal's
  validation layer then). A submit and a `Signal` commit it; a `Wait` stays in
  it, ahead of the work of whatever commits next.
- **Root signatures** are built once in the backend (`mtlb_root_signature_create`),
  which also reports each parameter's offset and size in the top-level argument
  buffer (from `IRRootSignatureGetResourceLocations`). The front-end uses those
  offsets, and `mtlb_pipeline_create` takes the root signature handle.
- **Shaders:** DXIL → Metal Shader Converter (`libmetalirconverter`) at PSO
  creation, against the root signature handle, producing a metallib loaded with
  `newLibraryWithData:`. Converted functions and reflection are cached per
  device (key: DXIL hash and size, root signature id, stage, entry); there is one
  `IRCompiler` per thread; equal depth-stencil descriptions share one
  `MTLDepthStencilState`.
- **Vertex input** goes through the converter's separate stage-in function:
  vertex shaders are compiled with `IRStageInCodeGenerationModeUseSeparateStageInFunction`,
  a stage-in function is synthesized with `IRMetalLibSynthesizeStageInFunction`
  from the D3D12 input layout (cached per layout on the vertex stage) and linked
  into the render pipeline with `vertexLinkedFunctions`. Vertex buffers and their
  strides reach it through an `IRRuntimeVertexBuffers` table bound at
  `kIRVertexBufferBindPoint`, so strides stay dynamic and no `MTLVertexDescriptor`
  is used.
- **Fence events:** `SetEventOnCompletion` registers a `notifyListener` on the
  fence's `MTLSharedEvent` through the bridge (`mtlb_event_notify`). The Metal
  listener block only records the notification in a bridge queue (mutex plus
  condition variable). One device-wide waiter thread, an ordinary thread of the
  front-end (a Windows thread in the PE build), blocks in `mtlb_notify_wait`
  with no timeout and signals the application events whose values were reached;
  pending waits are a `std::multimap` per fence keyed by value.
- **Residency:** every allocation is added to a per-device `MTLResidencySet`
  attached to the queue, so no per-draw `useResource` calls.

## Milestone 3 design

- **Interface versions.** `Device` implements `ID3D12Device10`; lists `ID3D12GraphicsCommandList7`.
  Objects carry a private IID (`internal_iid<T>()`): `ours<T>(p)` turns an application-supplied
  pointer into one of ours (QueryInterface plus release) so wrappers and foreign objects are
  rejected instead of cast. Pipeline state streams are parsed into the same description as the
  descriptor structs (`pipeline_stream.cpp`, with local mirrors of the newer subobject layouts).
  `CheckFeatureSupport` reports only what is implemented (feature level 12_0, shader model 6.6,
  resource binding tier 3, tiled resources none, ray tracing and mesh shaders none).
- **Descriptors.** A descriptor is the 24-byte `mtlb_descriptor` the converter's tables expect
  (buffer GPU address, texture view resource ID, metadata). Per-type increments differ
  (`descriptor_size()`). Descriptor creation happens in the front-end: textures and typed
  buffers create Metal views through the bridge (cached per resource and view description),
  and the result is written into the heap's CPU-visible memory. CBV/SRV/UAV heaps also keep a
  shadow `ViewInfo` table (resource, format, dimension, range): UAV clears need the resource a
  descriptor refers to, which the packed form does not hold. The device finds the heap behind
  a CPU handle through a registry keyed by CPU address. Null descriptors come from a per-device
  cache of views of a dummy resource.
- **Samplers.** Sampler heaps hold sampler IDs. Static samplers are not supported by the
  converter, so a root signature with static samplers gets an extra descriptor table appended
  (and the shader is compiled against the rewritten signature); the table lives in a device
  sampler heap created with the root signature.
- **Compute.** `Dispatch`, compute root arguments and compute PSOs mirror the graphics path
  (`RootState` per bind point, flushed lazily into argument-buffer snapshots in the stream).
  Compute encoders are `MTLDispatchTypeConcurrent`: the order between dispatches is up to the
  barriers. Empty dispatches are skipped (Metal validation rejects them).
- **Depth-stencil.** `SET_RENDER_TARGETS` carries a DSV and flags (read-only depth/stencil).
  The backend opens the render encoder with both attachments, clears become load actions and
  clear-only passes. Pipeline variants are keyed on the depth format.
- **Heaps and placed resources.** `Heap` owns an `MTLHeap` (untracked resources). A placed
  resource aliases the memory at its offset. D3D12 leaves such memory undefined until the
  application initialises it; render targets and depth-stencils with a clear value are
  cleared on first use by the queue (a pending-init list is turned into `CLEAR_RTV`/`CLEAR_DSV`
  records at the head of the first submit that references them), so titles that rely on the
  hardware behaviour of zeroed memory do not see garbage.
- **Barriers and hazards.** All resources are untracked (`MTLHazardTrackingModeUntracked`),
  since placed resources must be and GPU-address access cannot be tracked anyway. One
  `MTLFence` per queue is updated at the end of every encoder; the next encoder waits on it
  only when `sync_needed_` is set: after a barrier, at the start of a command list, and at the
  start of a submit. A barrier ends the open blit and render encoders (so their work is
  visible), and sets `sync_needed_`; compute encoders end at a barrier too. Transition barriers
  with equal states and `BEGIN_ONLY` split barriers are skipped; an aliasing barrier names both
  resources. Without barriers consecutive compute dispatches overlap, as in D3D12.
  `D3D12METAL_NO_BARRIERS` removes the waits (for finding missing barriers in a test).
- **Multiple queues.** Each queue has its own `MTLCommandQueue`, fence and open command buffer.
  Cross-queue `Signal`/`Wait` use `MTLSharedEvent` (a D3D12 fence is a shared event). Copy and
  compute queues run concurrently with the direct queue.
- **Large typed buffers.** Typed buffer SRVs/UAVs are texture-buffer views (Metal limit 2^28
  texels); structured and raw buffers are plain GPU addresses with no size limit.
- **ExecuteIndirect.** The command signature is validated at creation. Execution translates
  the application's argument buffer with an internal compute kernel (`translate_indirect`) into
  Metal indirect arguments plus the converter's draw-parameter structs (`IRRuntimeDraw*`
  overloads), reading the count buffer on the GPU and writing zero-instance draws for
  unused slots; constants, root views and vertex buffer views in the signature are applied by
  the same kernel into a per-command argument buffer.
- **Render passes.** `BeginRenderPass` records `SET_RENDER_TARGETS` (read-only depth from the flags), clears for
  CLEAR beginning accesses, and remembers the RESOLVE ending accesses; `EndRenderPass` records them as
  `ResolveSubresource`. The backend builds Metal passes from those records as for any other list.
- **Buffer lookup.** GPU addresses resolve through a sorted table of committed buffers and one buffer per heap
  covering the whole heap, so placed buffers that overlap never need a search among overlaps.
- **MSAA.** Multisampled textures and render targets with sample counts the device reports
  (`mtlb_device_caps::sample_counts`), `ResolveSubresource` through a render pass resolve or a
  blit for non-resolvable formats, multisample SRVs.
- **Queries.** Occlusion queries use the render pass visibility buffer (`setVisibilityResultMode` at
  `BeginQuery`/`EndQuery`); a query that spans several render passes gets one result slot per pass (8 per
  query), and `ResolveQueryData` sums them on the GPU (`resolve_occlusion`). A timestamp is a one-dispatch compute
  pass of its own with a counter sample at its end (Apple GPUs sample counters only at encoder boundaries).
  A blit in the same command buffer reads zeros for those samples, so `ResolveQueryData` of timestamps is
  performed by the command buffer's completion handler (a CPU copy from the sample buffer), which then signals
  an `MTLSharedEvent` that the queue's next command buffer waits for: nothing blocks the CPU or the queue lock.
  GPU timestamps use the `mach_absolute_time` scale, which `GetClockCalibration` pairs with `QueryPerformanceCounter`.
- **Markers.** `BeginEvent`/`EndEvent`/`SetMarker` become Metal debug groups.
  `WriteBufferImmediate` is an internal one-thread kernel after the preceding work.
- **Bundles.** A bundle is recorded like a direct list (without pipeline or root-signature
  checks, which it inherits) and `ExecuteBundle` appends its stream to the executing list,
  minus the initial `RESET_STATE`, copying the root state it left behind.

## Milestone 4 design

- **Descriptor writes are memory writes.** A `CreateXxxView`/`CreateSampler`/`CopyDescriptors*` call does no bridge
  call once its object has been seen: the DXGI format table is cached in the front-end (`formats.cpp`), raw and
  structured buffer views are an address plus a size computed in the front-end (the metadata word is the byte
  size; `tests/test_descriptors.cpp` compares it with the backend's `mtlb_buffer_view`), typed buffer views and
  texture views are memoized per resource and view description (the bridge creates the Metal view object once),
  and samplers are memoized per description in the device. The front-end does no locking on the common path: the
  destination handle is checked by a thread-local last-heap cache (`Device::validate_cpu_range`, generation
  counters shared by all devices so a recycled address never matches). Measured with `p_descbench` (ns per
  descriptor, Wine / Rosetta; before to after): texture SRV 753 to 36, structured buffer SRV 395 to 17, typed buffer
  SRV 1526 to 37, UAV buffer 430 to 55, UAV texture 824 to 83, sampler 422 to 43, RTV 758 to 16, CBV 6, copies 2.
- **Shader cache.** Converted shaders and their reflection, and stage-in functions, are stored on disk
  (`src/bridge/metal/disk_cache.mm`) under `~/Library/Caches/d3d12metal/<exe name>/` (`D3D12METAL_CACHE_DIR`
  replaces the path, `D3D12METAL_CACHE=0` disables it, `D3D12METAL_CACHE_MAX_MB` sets the limit, 1024 by default).
  The key is the SHA-256 of the DXIL, the root signature blob, the stage and entry point, the converter settings,
  the converter library (path, size and modification time) and a cache revision; stage-in entries add the vertex
  stage's key and the input layout. Entries carry a header, the key and a payload checksum, are written to a
  temporary file and renamed into place, and a damaged entry is deleted and rebuilt. When the directory exceeds its
  limit the oldest files (a hit refreshes the modification time) are deleted down to 80% of it. `mtlb_cache_get_stats`
  reports hits, misses, writes, corrupt entries and evictions. Godot's 210 pipelines: 1169 ms cold, 82 ms warm.
  PSO creation is thread-safe: one converter per thread, locks only around the maps; `p_psothreads` creates 288
  pipelines from 8 threads. `MTLBinaryArchive` was not added: Metal's own pipeline cache already makes repeated
  identical libraries cheap (the warm number above includes `newRenderPipelineState`).
- **Hardening.** The backend range-checks copies, resolves and render target views against the textures and buffers
  they name (`range_fits`, `box_fits`, `copy_region_bytes` in `queue.mm`; a bad record is skipped and reported).
  CPU descriptor handles are validated against the registered heaps (type, bounds, alignment) before any read or
  write. RTV/DSV descriptors hold an id, not a pointer: `Device::acquire_attachment` turns it into a resource with a
  new reference (or none when the resource is gone, which makes the view null), and creation refuses subresources
  that do not exist. Command lists hold references to the pipelines, resources, query heaps, descriptor heaps,
  signatures and render targets their stream names (`ObjectRefs`, released at `Reset` or destruction), so a
  released object cannot be freed under a recorded list. Samplers are clamped to legal ranges before they reach Metal.
- **Statistics.** `D3D12METAL_STATS=1` prints, every 120 presents, the per-frame averages of submits, command lists,
  command buffers, render passes, compute and blit encoders, barriers, fence syncs, descriptor writes, PSO creations,
  stream bytes and unix calls, and the total PSO creation time (`src/common/stats.cpp`; the backend side is
  `mtlb_stats_get`). With the variable unset each counter is one predictable branch.
- **Bridge crossings per frame** (Godot scene under Wine, each about 0.35 us): `mtlb_queue_submit` (one per
  `ExecuteCommandLists`), `mtlb_queue_signal` and `mtlb_queue_wait` (fences), `mtlb_queue_present`,
  `mtlb_event_completed_value` (`GetCompletedValue`, about 0.5 per frame; a shared-memory mirror was not built:
  a listener per value would be needed and the cost is below 1 us per frame), and the object destroys of resources the
  application churns. `Map`/`Unmap`/`GetGPUVirtualAddress` never cross. `QueryVideoMemoryInfo` (polled every frame)
  used to cross every time and is answered from a 250 ms cache. The Godot scene makes 9 unix calls per frame.

## Milestone 6 design

- **Geometry and tessellation emulation.** A pipeline with a GS, or an HS and DS, becomes a Metal *mesh* pipeline built with the converter's runtime
  (`IRRuntimeNewGeometryEmulationPipeline`, `IRRuntimeNewGeometryTessellationEmulationPipeline`): the vertex shader runs as the object function
  (plus the hull shader and tessellator for patches), the geometry or domain shader as the mesh function. All stages are converted with a second
  per-thread compiler that has `IRCompilerEnableGeometryAndTessellationEmulation` on, and with the topology type of the pipeline
  (`IRCompilerSetInputTopology`); the stage-in function is synthesized for the input layout (an empty layout for a vertex shader that pulls its
  data from buffers). Stages keep their `MTLLibrary` (functions with constants are instantiated by the runtime) and the reflection scalars the
  runtime configuration needs (`ShaderStage`, `EmulatedPipeline`). The disk cache key of such a stage includes the emulation flag and topology.
- **Binding and drawing.** The replay binds root arguments (at the argument buffer and hull/domain points), descriptor heaps and the
  `IRRuntimeVertexBuffers` table on the object and mesh stages instead of the vertex stage, a zero buffer for what the application did not set (the
  converted stages read their argument buffers regardless), and the tessellator tables (`IRRuntimeLoadTessellatorTables`) for patch pipelines.
  Draws go through `IRRuntimeDraw[Indexed]PrimitivesGeometryEmulation` / `IRRuntimeDraw[Indexed]PatchesTessellationEmulation` (the index buffer's
  view offset is folded into the first index). Adjacency (`*_ADJ`) and patch-list topologies are recorded; a plain pipeline skips draws that use
  them, an emulated one skips draws whose topology does not match (patch lists need the hull shader's control point count).
- **Statistics.** `mtlb_stats` carries the GPU time of completed command buffers; `D3D12METAL_STATS=1` prints it with the frame rate.

## Wine build

Under Wine the layer is two halves in one process (one address space):

```
 game.exe ── d3d12.dll / dxgi.dll   x86-64 PE, MinGW: front-end + mtlb client
                │  __wine_unix_call_dispatcher(handle, index, params)
                ▼
            d3d12metal.so           x86-64 macOS (Rosetta): __wine_unix_call_funcs[]
                                    + the Metal backend
```

- **Transport.** `tools/gen_mtlb_wine.py` reads the `MTLB_EXPORT` prototypes of
  `mtlb.h` and generates, at build time, the function indices, one parameter
  struct per function (arguments, then the result), the PE definitions of every
  `mtlb_*` function (pack, dispatch, unpack) and the unix call table. There is
  no hand-written thunk list: a function added to `mtlb.h` crosses the boundary
  on the next build. Pointers (mapped buffer memory, command stream spans, DXIL
  blobs) pass unchanged, handles are plain 64-bit values, and a `const char *`
  result (`mtlb_last_error`) is copied into the parameter struct. Every unix
  entry runs in an `@autoreleasepool`. Blocking calls (notification wait, CPU
  fence wait) block inside the unix call; the waiter thread is an ordinary
  Windows thread.
- **Loading.** On the first `mtlb_*` call (not in `DllMain`) the PE client loads
  `d3d12metal.so` with `NtQueryVirtualMemory(MemoryWineLoadUnixLibByName = 1002)`:
  first `x86_64-unix/d3d12metal.so` next to `d3d12.dll` (explicit path), then the
  name `d3d12metal` searched along `WINEDLLPATH`. The result's second word is the
  table handle passed to `__wine_unix_call_dispatcher` (an exported data pointer
  of ntdll). Not running under Wine, or no `.so`, is reported once on stderr and
  the calls fail with `MTLB_ERROR_DEVICE`.
- **PE DLLs.** Everything is in `d3d12.dll` (the swap chain needs the D3D12
  classes); `dxgi.dll` has no code, its `CreateDXGIFactory*` exports forward to
  `d3d12.dll`. Both link libstdc++, libgcc and winpthread statically (the .def
  files in `src/pe/` list the exports). Wine's own `d3d12.dll`/`dxgi.dll` are
  replaced by native overrides (`WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n"`)
  with our DLLs next to the program or in `system32`.
- **Headers.** The PE build uses MinGW's `d3d12.h` and `dxgi1_6.h`: they declare
  the aggregate-returning methods (`GetDesc`, `GetCPUDescriptorHandleForHeapStart`,
  `GetResourceAllocationInfo`, `GetAdapterLuid`, ...) in the explicit form
  `T *Name(T *ret, ...)`, the convention Windows callers (and vkd3d) use, which
  GCC does not produce for a by-value return. `D3D12M_AGGREGATE_RETURN`
  (`d3d12/object.h`) picks the form per build. The native build keeps
  DirectX-Headers and the hand-written `dxgi_interfaces.h`.
- **Windows.** Wine's macOS driver keeps a `WineWindow` per top-level window;
  its content view creates a `WineMetalView` backed by a `CAMetalLayer`
  (`-newMetalViewWithDevice:`). This build of `winemac.so` exports no `macdrv_*`
  functions, so `wine_window.mm` finds the window through the Objective-C runtime
  (`NSApp.windows`, `-hwnd`) on the main thread. The PE side passes
  `GetAncestor(hwnd, GA_ROOT)`; the layer provider is installed by a constructor
  of the unix module (`src/bridge/metal/layer_provider.h`). The native build has
  no provider (tests install one returning a detached layer).
- **Threads.** Metal calls back on its own threads, which have no Windows
  thread state: listener blocks only touch the bridge's notification queue.
- **Tracing.** `D3D12METAL_LOG=1` prints every call with its arguments and result
  on both sides (`[pe]`, `[unix]`).

## Swap chains

`IDXGISwapChain4` (`src/dxgi/swapchain.cpp`) owns ordinary D3D12 textures as back
buffers (render-target usage, also readable as a texture) and, through the
bridge, a `CAMetalLayer` configured for the window (`mtlb_swapchain_create`).
`Present` calls `mtlb_queue_present`: after the queue's earlier work the backend
encodes a fullscreen-triangle pass that samples the back buffer into the layer's
next drawable, then `presentDrawable` and commit, in the queue's command buffer.
One pass covers every format difference (RGBA/BGRA order, sRGB encoding) and
scaling; layer formats are BGRA8, BGRA8 sRGB, RGB10A2 and RGBA16F, chosen from the
back buffer format. `SyncInterval` 0 turns `displaySyncEnabled` off; any other
value presents on the next vsync (`nextDrawable` blocks when all drawables are in
flight, which paces the application). Flip models cycle the back buffer index at
each present; blit models stay on buffer 0. The frame latency waitable object is
a real event, signalled by the device's fence waiter when a presented frame has
finished (it starts signalled). `ResizeBuffers` recreates the back buffers and
changes the layer's drawable size and format. Windowed mode only:
`SetFullscreenState` is accepted and ignored. Outputs: one `IDXGIOutput6`, its
display modes from `EnumDisplaySettings` under Wine.

`D3D12METAL_DUMP_PRESENT=<file.png>` (with `D3D12METAL_DUMP_PRESENT_FRAME=N`,
default 30) renders the Nth present a second time into a readable texture and
writes it as a PNG: the verification path where no screenshot can be taken.

## Layout

| Path | Contents |
|---|---|
| `src/bridge/mtlb.h` | Bridge C API, handles, POD descriptors |
| `src/bridge/mtlb_cmd.h` | Command stream encoding |
| `src/bridge/metal/` | Metal backend (Objective-C++), including swap chains |
| `src/bridge/wine/` | Wine transport: PE client, unix table (generated from `mtlb.h`), window lookup |
| `src/pe/` | `d3d12.dll` / `dxgi.dll` build (module definitions, forwarder) |
| `cross/` | Meson cross files: MinGW (PE) and x86-64 macOS (unix module) |
| `tools/` | `build-wine.sh`, `run-wine-tests.sh`, the transport generator, PNG and window helpers |
| `src/d3d12/` | `ID3D12*` COM implementations |
| `src/dxgi/` | `IDXGI*` COM implementations |
| `tests/` | Native headless tests (render offscreen, read back, compare) |
| `tests/wine/` | Win32 test programs run under Wine (MinGW): `wine_basic`, `swapchain_test`, `hello_triangle` |
| `tests/portable/` | Tests that build both natively and as Win32 programs (`t12.h`), one per feature area |
| `tests/shaders/` | HLSL, compiled to DXIL with DXC at build time |
