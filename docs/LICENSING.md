# Licensing and redistribution

This project is licensed **LGPL-2.1-or-later** (`LICENSE`). Every source file carries
`SPDX-License-Identifier: LGPL-2.1-or-later`. The "or later" matters: the GDeflate decompressor port is derived from
Apache-2.0 code, which is compatible with LGPL-3.0 but not with version 2.1 alone.

This document says what can be shipped in a release (a `.app` or archive), what must be fetched by the user, and why.
It is a good-faith engineering summary, not legal advice; get a lawyer's opinion before a commercial release.

| Component | Licence | Ship in our release? | How the user gets it |
|---|---|---|---|
| d3d12-metal (layer, CLI, app) | LGPL-2.1-or-later | yes | our release |
| `libdxilconv.dylib` (Microsoft DXC `dxilconv`) | NCSA / LLVM | yes, with the licence text (`licenses/`) | in the release |
| GDeflate port (Microsoft DirectStorage reference) | Apache-2.0 | yes (compiled into the layer; notice kept) | in the release |
| libdeflate-gdeflate | MIT | tests only, not shipped | - |
| DirectX-Headers | MIT | build dependency only | `brew install directx-headers` |
| MinGW-w64 runtime pieces in the PE DLLs | ZPL/PD/BSD | yes (inside the DLLs) | in the release |
| **Apple Metal Shader Converter** (`libmetalirconverter.dylib`) | Apple proprietary | **no** | the user installs it from Apple |
| **Wine** (Gcenx macOS build) | LGPL-2.1+ (plus bundled parts) | not bundled; downloaded | the app downloads the pinned release and verifies its SHA-256 |
| **Steam client** | Valve proprietary | **no** | the app downloads Valve's installer on request |
| Games | proprietary | **never** | the user's own Steam library |

## Metal Shader Converter (MSC)

What we know:

- The headers (`/usr/local/include/metal_irconverter/`) carry the Apache-2.0 licence, so building against them and
  describing the API is fine.
- The library `libmetalirconverter.dylib` and the `metal-shaderconverter` tool are distributed by Apple through the
  Apple Developer downloads page (<https://developer.apple.com/metal/shader-converter/>) and installed by an Apple
  `.pkg`. Apple's pages publish **no** statement that the library may be redistributed by third parties; the download is covered by the
  agreement the downloader accepts (Apple Developer Agreement / the licence shown by the installer). A developer asking exactly this
  on the Apple Developer Forums (thread 829379) had no official answer when we looked.
- Apple's Game Porting Toolkit, which also contains Apple libraries, restricts redistribution to its own terms.

Decision: **treat it as not redistributable.** The layer links to it dynamically (rpath `/usr/local/lib`, where Apple's installer puts it) and the app has a setup step that detects it and, when missing, tells the user to download it from Apple
(button opens the page; the user installs the `.pkg`). `d3d12metal doctor` checks for it. If Apple grants written permission to
redistribute it, the only change needed is to copy the dylib into the app bundle and add its directory to the rpath of `d3d12metal.so`.

Practical consequence: the x86-64 slice is needed (Wine runs under Rosetta). Apple's dylib is universal, so no
extra step is needed.

## Wine

Wine is LGPL-2.1-or-later. Redistributing Wine builds is allowed if we ship the licence and offer the source; the
Gcenx builds are already published that way (<https://github.com/Gcenx/macOS_Wine_builds>, sources at
<https://gitlab.winehq.org/wine/wine>). We prefer **downloading the pinned release on first run** over bundling:
the release then stays Gcenx's responsibility, the app stays small, and our binary needs no extra source offer. The
download is verified against a SHA-256 pinned in the app (`Sources/Core/RuntimeManager.swift`). Our layer is a separate DLL pair plus a unix
library loaded through Wine's documented PE/unix mechanism; we copy no Wine code.

Gcenx builds also bundle components with their own licences (e.g. FreeType, MoltenVK, SDL); they live inside the downloaded app and
come with their own notices.

## Steam and games

We never ship Steam or any game file. Installing Steam downloads Valve's `SteamSetup.exe` from Valve's CDN at the
user's request. Game profiles contain only public facts (app ids, executable names, option values, notes); the repository must
not contain game binaries, assets, screenshots or captures that show game content.

Compatibility layers like this one are widely distributed (vkd3d-proton, DXVK, Wine), and the layer reimplements public
Microsoft API specifications. It is clean-room: no Microsoft D3D12 binary is used. Note that the Windows `d3d12.dll` of the
game's own platform must not be copied; the layer replaces it.

## Publishing checklist

- [x] LICENSE and SPDX headers
- [x] THIRD_PARTY_NOTICES.md for everything compiled in
- [x] No personal paths, ids or addresses in the repository (`tools/check.sh` greps for them)
- [ ] Release archive includes `LICENSE`, `THIRD_PARTY_NOTICES.md`, `licenses/`
- [ ] Decide on a name (the app name is the single constant `AppInfo.name`; check trademark issues with "Metal" and "D3D12")
