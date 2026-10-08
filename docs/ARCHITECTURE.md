# Architecture

```
 Game (x86-64 PE, under Wine + Rosetta)
   │  D3D12 / DXGI COM calls
   ▼
 d3d12.dll / dxgi.dll  ── "front-end": portable C++ (src/d3d12, src/dxgi)
   │  object create/destroy + one submit per ExecuteCommandLists (one span per list)
   ▼
 mtlb bridge C API (src/bridge/mtlb.h)  ── POD structs + opaque handles only
   │  native build: direct call    │  Wine build: __wine_unix_call thunk
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
- **One queue synchronisation mechanism.** A queue keeps one open, uncommitted
  `MTLCommandBuffer` (retaining its references: applications release objects as
  soon as a fence is signalled, possibly before the buffer retires, and
  `commandBufferWithUnretainedReferences` trips Metal's validation layer then). Submits and `Wait` append to it; `Signal` appends and
  commits, so Execute followed by Signal costs one commit. It is also committed
  after 32 submits and when the queue is destroyed.
- **GPU virtual addresses are Metal `gpuAddress`.** `D3D12_GPU_VIRTUAL_ADDRESS`
  for buffers is the backing `MTLBuffer.gpuAddress` (+offset). Shader-visible
  descriptor heaps are `MTLBuffer`s of `IRDescriptorTableEntry`, so
  `D3D12_GPU_DESCRIPTOR_HANDLE.ptr` is a GPU address too.
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

## Layout

| Path | Contents |
|---|---|
| `src/bridge/mtlb.h` | Bridge C API, handles, POD descriptors |
| `src/bridge/mtlb_cmd.h` | Command stream encoding |
| `src/bridge/metal/` | Metal backend (Objective-C++) |
| `src/d3d12/` | `ID3D12*` COM implementations |
| `src/dxgi/` | `IDXGI*` COM implementations |
| `tests/` | Native headless tests (render offscreen, read back, compare) |
| `tests/shaders/` | HLSL, compiled to DXIL with DXC at build time |
