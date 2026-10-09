# Third-party notices

## GDeflate (DirectStorage reference implementation)

Used in: `src/bridge/metal/gdeflate_kernel.msl.inc` (the GPU decompressor behind the DirectStorage meta command, a port
to the Metal Shading Language of the reference HLSL decompressor), `tests/portable/gdeflate_ref.h` (the layout of the
tile streams the tests make).

Source: <https://github.com/microsoft/DirectStorage>, `GDeflate/shaders/GDeflate.hlsl`, `GDeflate/shaders/tilestream.hlsl`
and `GDeflate/GDeflate/GDeflateCompress.cpp`.

```
SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
SPDX-License-Identifier: Apache-2.0
```

Licensed under the Apache License, Version 2.0 (full text: `third_party/licenses/Apache-2.0.txt`). The MSL port
changes the algorithm's data structures to SIMD-group registers and threadgroup memory of Metal, adds bounds checks on
every access, a tile scheduler (one SIMD group per tile of any stream, with a prefix sum of the tile counts) instead of
the reference's work-stealing loop, and handles damaged streams.

The DirectStorage repository itself is MIT licensed (Copyright (c) Microsoft Corporation;
`third_party/licenses/DirectStorage-MIT.txt`).

## libdeflate with GDeflate support (tests only)

Used in: `third_party/libdeflate-gdeflate/` (the reference GDeflate compressor, built into the tests only, never into
the layer).

Source: <https://github.com/NVIDIA/libdeflate>, branch `gdeflate` (a fork of <https://github.com/ebiggers/libdeflate>).
Only the files needed to compress and decompress GDeflate were copied, unmodified.

```
Copyright 2016 Eric Biggers (MIT, see third_party/libdeflate-gdeflate/COPYING)
SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0   (NVIDIA's GDeflate changes; third_party/licenses/Apache-2.0.txt)
```

## Interface facts from vkd3d-proton

The DirectStorage meta command's id, parameter lists and structures (`src/d3d12/meta_command.h`) are facts about an
interface shared by DirectStorage and GPU drivers, taken from the documentation in vkd3d-proton
(<https://github.com/HansKristian-Work/vkd3d-proton>, LGPL-2.1, `libs/vkd3d/meta_commands.c`). No code or shader of
vkd3d-proton was copied.

## Microsoft DirectX-Headers (build dependency)

Used in: the native (arm64) build and its tests (`d3d12.h`, `dxgi*.h`, `libDirectX-Guids`, from `brew install directx-headers`).
The Wine build uses the MinGW-w64 headers instead. Source: <https://github.com/microsoft/DirectX-Headers>, MIT License,
Copyright (c) Microsoft Corporation. Not copied into this repository.

## Microsoft DirectX Shader Compiler: dxilconv (shipped as `libdxilconv.dylib`)

Used in: `third_party/dxilconv` (a build recipe and a header shim; the sources are DirectXShaderCompiler's
`projects/dxilconv`, taken unmodified from a checkout by `tools/build-dxilconv.sh`). The built `libdxilconv.dylib`
(DXBC to DXIL, loaded on demand for Shader Model 4/5 shaders) is **part of what the layer ships**.
Source: <https://github.com/microsoft/DirectXShaderCompiler>. Licence: University of Illinois/NCSA Open Source License
(DXC's `LICENSE.TXT`; DXC is derived from LLVM, whose newer code is Apache-2.0 with LLVM exceptions). The licence text
must accompany binary redistributions: it is installed as `licenses/DirectXShaderCompiler-LICENSE.TXT` next to the
dylib by the app and is reproduced in the release archive.
The DXC compiler (`dxc`) itself is only a build/test tool (HLSL to DXIL for the test shaders) and is not shipped.

## Wine (runtime, downloaded, not part of this repository)

The layer runs in Wine (LGPL-2.1-or-later, <https://www.winehq.org>) through Wine's public unix-call interface. The app
downloads the Gcenx macOS build of Wine (<https://github.com/Gcenx/macOS_Wine_builds>) on first use rather than bundling it. Wine
builds contain further components under their own licences (FreeType, MoltenVK, SDL, ...): see the licence files inside the
downloaded `Wine *.app`. No Wine source is copied into this repository (`tests/wine` programs use only the public Win32/COM APIs).

## MinGW-w64 (linked into the PE DLLs)

`d3d12.dll`, `dxgi.dll` and the Windows test programs are built with MinGW-w64 and statically link its C runtime pieces
(permissive: Zope Public License 2.1, public domain and BSD-style notices; <https://www.mingw-w64.org>).

## Apple Metal Shader Converter (NOT redistributed)

The backend links dynamically against `libmetalirconverter.dylib` and uses its headers (`metal_irconverter*.h`, Apache-2.0,
Copyright (c) Apple Inc.; they are not copied here). The library itself is Apple's proprietary software and is **not**
shipped, bundled or downloaded by this project: every user installs it from Apple (see docs/LICENSING.md).

## Valve Steam client (NOT redistributed)

The app can install the Windows Steam client into a bottle. It downloads `SteamSetup.exe` from Valve's official CDN on the
user's request; the user accepts Valve's Steam Subscriber Agreement. Nothing of Steam is bundled. The small
`steamwebhelper` shim the app compiles into the bottle is original code (LGPL-2.1-or-later).

## Fonts and images

The in-game HUD uses an 8x8 bitmap font generated by this project (`src/bridge/metal/hud_font.h`), drawn from scratch.
The app uses SF Symbols through SwiftUI system APIs only (no Apple art is embedded).
