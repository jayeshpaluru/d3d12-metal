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
| `tests/shaders/` | HLSL, compiled to DXIL with DXC at build time |
