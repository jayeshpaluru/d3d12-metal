# Status

Milestone 1 (native arm64 macOS build, headless tests). "Stubbed" methods exist
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

## DXGI

| Object | Implemented | Stubbed |
|---|---|---|
| `IDXGIFactory` .. `IDXGIFactory5` | `EnumAdapters`, `EnumAdapters1`, `EnumAdapterByLuid`, `EnumWarpAdapter` (unsupported), `IsCurrent`, `CheckFeatureSupport` (tearing), `MakeWindowAssociation`, `GetWindowAssociation`, `GetParent`, private data | swap chain creation, software adapter, status/occlusion registration |
| `IDXGIAdapter` .. `IDXGIAdapter3` | `GetDesc`, `GetDesc1`, `GetDesc2`, `CheckInterfaceSupport`, `QueryVideoMemoryInfo`, `SetVideoMemoryReservation`, `EnumOutputs` (none), private data | budget-change notifications |

## D3D12

| Object | Implemented | Stubbed |
|---|---|---|
| `ID3D12Device` .. `ID3D12Device2` | `CreateCommandQueue`, `CreateCommandAllocator`, `CreateGraphicsPipelineState`, `CreateCommandList`, `CreateDescriptorHeap`, `GetDescriptorHandleIncrementSize`, `CreateRootSignature`, `CreateConstantBufferView`, `CreateRenderTargetView`, `CopyDescriptors`, `CopyDescriptorsSimple`, `CreateCommittedResource`, `CreateFence`, `GetCopyableFootprints`, `GetResourceAllocationInfo`, `CheckFeatureSupport` (OPTIONS, OPTIONS1, ARCHITECTURE, ARCHITECTURE1, FEATURE_LEVELS, FORMAT_SUPPORT, MULTISAMPLE_QUALITY_LEVELS, GPU_VIRTUAL_ADDRESS_SUPPORT, SHADER_MODEL, ROOT_SIGNATURE), `GetNodeCount`, `GetAdapterLuid`, `GetDeviceRemovedReason`, `MakeResident`, `Evict`, private data, `SetName` | compute PSOs, heaps, placed/reserved resources, SRV/UAV/DSV/sampler creation, query heaps, command signatures, shared handles, pipeline libraries, `CreatePipelineState`, resource tiling |
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

## Known limitations

- Resource barriers are not translated. Metal does not track hazards for
  resources reached only through GPU addresses (CBV tables, root descriptors),
  so a copy that feeds a later draw through such a resource needs explicit
  synchronisation in a later milestone.
- Textures exist only in DEFAULT heaps (private storage). Depth-stencil targets
  cannot be bound yet: the command stream and the replay have no depth plumbing
  (`SET_RENDER_TARGETS` carries colour views only, there is no `CLEAR_DSV`); it
  comes with the DSV milestone, together with a test that exercises it. Draws
  with no render targets are skipped with a one-time message.
- The native build uses by-value struct returns for methods such as `GetDesc`;
  the Win32 (MinGW/Wine) ABI variants are not handled.
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

Wine notes for the next milestone:

- `FenceWaiter::run` must run on a Windows thread (a `std::thread` built for the PE target is one), never on a Metal callback thread. The listener blocks in `notify.mm` only lock a mutex, queue a record and notify a condition variable.
- `platform_set_event` calls `SetEvent` in `_WIN32` builds; the native build uses the small event object in `src/common/platform.cpp`.
