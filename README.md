# d3d12-metal

A from-scratch Direct3D 12 → Metal translation layer for running Windows D3D12 games on Apple Silicon Macs under Wine.

The layer ships replacement `d3d12.dll` / `dxgi.dll` that implement the D3D12/DXGI APIs on top of Metal. Games call into it without modification — no game code is decompiled or redistributed.

**Target:** Marvel's Spider-Man Remastered (D3D12) on an M2 Max.

## Architecture

| Component | D3D12 concept | Metal mapping |
|---|---|---|
| Swapchain | `IDXGISwapChain`, present | `CAMetalLayer` + drawables |
| Resources | heaps, placed resources | `MTLHeap`, `MTLBuffer`, `MTLTexture` |
| Descriptors | descriptor heaps, root signatures | argument buffers (bindless) |
| Pipelines | PSOs | `MTLRenderPipelineState`, depth/stencil states |
| Shaders | DXIL | Apple Metal Shader Converter → metallib |
| Commands | command lists/queues, barriers | `MTLCommandBuffer` + encoders |
| Sync | fences | `MTLSharedEvent` |
| Wine bridge | PE-side DLL | Unix-side dylib thunks |

## Milestones

1. **Toolchain + triangle:** Microsoft's D3D12 HelloTriangle renders through Metal under Wine
2. **D3D12 samples:** textures, CBVs, bindless, compute, MSAA
3. **Game boots:** API tracing, menus render
4. **In-game rendering:** correctness (barriers, formats, shader edge cases)
5. **Performance:** PSO caching, encoder batching, MetalFX upscaling

## Prerequisites

- macOS on Apple Silicon (arm64) with the Xcode Command Line Tools
- [Metal Shader Converter](https://developer.apple.com/metal/shader-converter/) (headers in `/usr/local/include`, `libmetalirconverter.dylib` in `/usr/local/lib`)
- `brew install meson ninja directx-headers` (DirectX-Headers provides `d3d12.h`)
- A DXC build (HLSL to DXIL) for the test shaders; found on `PATH`, or pass it explicitly, e.g. `meson setup build -Ddxc=/Users/jsp/code/deps/dxc-build/bin/dxc`
- For the later Wine milestones: `brew install --cask wine-stable` and `brew install mingw-w64`

## Building and testing

```sh
meson setup build -Ddxc=/Users/jsp/code/deps/dxc-build/bin/dxc   # omit if dxc is on PATH
meson compile -C build
meson test -C build
```

If DirectX-Headers is not installed under `/opt/homebrew` and `pkg-config` is unavailable, pass `-Ddirectx_headers_prefix=<prefix>` to `meson setup`.

The tests are headless: they render offscreen on the default Metal device and read the pixels back.

## Prior art

- [DXMT](https://github.com/3Shain/dxmt): D3D11 → Metal
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton): D3D12 → Vulkan
- Apple D3DMetal (Game Porting Toolkit)

## Legal

This project contains no game code or assets. Never commit game binaries, assets, dumps or captures.
