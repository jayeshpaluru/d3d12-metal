# Architecture

```
 Game (x86-64 PE, under Wine + Rosetta)
   │  D3D12 / DXGI COM calls
   ▼
 d3d12.dll / dxgi.dll  ── "front-end": portable C++ (src/d3d12, src/dxgi)
   │  object create/destroy + one submit per ExecuteCommandLists
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
  hands the whole stream to the backend in one call, which replays it into a
  `MTLCommandBuffer`. Under Wine that means one unix call per submit.
- **GPU virtual addresses are Metal `gpuAddress`.** `D3D12_GPU_VIRTUAL_ADDRESS`
  for buffers is the backing `MTLBuffer.gpuAddress` (+offset). Shader-visible
  descriptor heaps are `MTLBuffer`s of `IRDescriptorTableEntry`, so
  `D3D12_GPU_DESCRIPTOR_HANDLE.ptr` is a GPU address too.
- **Shaders:** DXIL → Metal Shader Converter (`libmetalirconverter`) at PSO
  creation, with the root signature, producing a metallib loaded with
  `newLibraryWithData:`. The top-level argument buffer layout follows the root
  signature, per the Metal Shader Converter user manual.
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
