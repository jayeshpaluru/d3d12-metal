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
- For the Wine build: `brew install mingw-w64` (the PE DLLs and Win32 test programs), a Wine 11 for macOS (x86-64, runs under Rosetta 2; developed against the Gcenx build 11.18), Rosetta 2, and a Metal Shader Converter dylib that includes the x86-64 slice (the shipped one is universal)

## Building and testing

```sh
meson setup build -Ddxc=/Users/jsp/code/deps/dxc-build/bin/dxc   # omit if dxc is on PATH
meson compile -C build
meson test -C build
```

### Wine build

The Wine flavour is two cross builds, `d3d12.dll` + `dxgi.dll` (x86-64 PE, MinGW) and `x86_64-unix/d3d12metal.so` (x86-64 macOS, loaded by Wine's unix loader), plus the Win32 test programs:

```sh
tools/build-wine.sh --dxc /Users/jsp/code/deps/dxc-build/bin/dxc    # -> build-wine/out/
tools/run-wine-tests.sh                                             # wine_basic, swapchain_test, hello_triangle
```

`tools/run-wine-tests.sh` reads `WINE_ROOT` (the directory with `bin/wine`) and `WINEPREFIX`; the defaults are the paths of the development machine. It reports PASS/FAIL per test, saves the presented frame and, when macOS permits, a window screenshot under `build-wine/screens/`, and logs under `build-wine/logs/`. To run a program by hand:

```sh
cd build-wine/out
export WINEPREFIX=<prefix> WINEDLLPATH=$PWD WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n"
<wine>/bin/wine hello_triangle.exe --frames 300     # or: --selftest, wine_basic.exe, swapchain_test.exe
```

To use the layer with another program put `d3d12.dll` and `dxgi.dll` next to its exe (or in the prefix's `system32`), and keep `x86_64-unix/d3d12metal.so` next to `d3d12.dll` or in a directory listed in `WINEDLLPATH`. `D3D12METAL_LOG=1` traces every bridge call on both sides of the transport on stderr.

#### Godot 4 (D3D12 renderer) under Wine

`tests/godot/project` is a small 3D scene (box, textured sphere, shadowed directional light, sky, a label); its script renders
N frames, prints the frame rate, saves a screenshot and quits. Download the official Windows build (untrusted: only run it under Wine):

```sh
mkdir -p /Users/jsp/code/deps/godot && cd /Users/jsp/code/deps/godot
gh release download 4.7.2-stable -R godotengine/godot -p 'Godot_v4.7.2-stable_win64.exe.zip' -D . && unzip Godot_v4.7.2-stable_win64.exe.zip
cd - && tools/build-wine.sh --dxc <dxc> && tools/run-godot-test.sh [--method forward_plus|mobile] [--frames N] [--stats]
```

The runner copies the DLLs next to Godot, runs `--rendering-driver d3d12`, validates the screenshot
(`tools/check_godot_screenshot.py`) and writes `build-wine/screens/godot-<method>.png` and `build-wine/logs/godot-<method>.log`.
Forward+ passes; Mobile does not render yet (see docs/STATUS.md).

#### Environment variables

| Variable | Effect |
|---|---|
| `D3D12METAL_LOG=1` | trace every bridge call (both sides) |
| `D3D12METAL_STATS=1` | per-frame counters every 120 presents (submits, encoders, barriers, syncs, descriptor writes, PSO time, unix calls) |
| `D3D12METAL_CACHE_DIR`, `D3D12METAL_CACHE=0`, `D3D12METAL_CACHE_MAX_MB` | shader disk cache location, off switch, size limit (default `~/Library/Caches/d3d12metal/<exe>`, 1024 MB) |
| `D3D12METAL_DUMP_FAILED=<dir>` | keep DXIL the shader converter rejects (inspect with `dxc -dumpbin`) |
| `D3D12METAL_DUMP_PRESENT=<png>` | write the Nth presented frame |

If DirectX-Headers is not installed under `/opt/homebrew` and `pkg-config` is unavailable, pass `-Ddirectx_headers_prefix=<prefix>` to `meson setup`.

The tests are headless: they render offscreen on the default Metal device and read the pixels back.

## Prior art

- [DXMT](https://github.com/3Shain/dxmt): D3D11 → Metal
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton): D3D12 → Vulkan
- Apple D3DMetal (Game Porting Toolkit)

## Legal

This project contains no game code or assets. Never commit game binaries, assets, dumps or captures.
