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
