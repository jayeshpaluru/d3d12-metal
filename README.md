# D3D12 Metal

Play Windows **Direct3D 12** games on Apple Silicon Macs. A from-scratch Direct3D 12 and DXGI implementation on top of Metal that runs inside
[Wine](https://www.winehq.org), with a native Mac app and a command line tool to set everything up: Wine, a Steam bottle, the layer in each game, per-game settings and
diagnostics.

The layer ships replacement `d3d12.dll` / `dxgi.dll`; games run unmodified. No game code or assets are part of this project.

> **Status: early.** Spider-Man Remastered is playable (about 55 to 60 fps on an M2 Max). Other games range from "boots" to "untested": see
> [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). Expect rough edges. The working name "D3D12 Metal" is a single constant and may change.

![Library](docs/images/library.png)

| | |
|---|---|
| ![A game's page](docs/images/game.png) | ![Setup assistant](docs/images/setup.png) |
| ![Performance HUD](docs/images/hud-hello-triangle.png) | ![Health check](docs/images/health.png) |

## Requirements

- A Mac with **Apple Silicon** and **macOS 14 or newer** (macOS 15+ recommended: Rosetta's AVX2 support, which most modern games require, arrived there).
- **Rosetta 2** (`softwareupdate --install-rosetta`). Wine runs as x86-64 code.
- **Apple's Metal Shader Converter**, installed from [Apple's developer site](https://developer.apple.com/metal/shader-converter/). Apple's terms do not let us include it, so
  every user installs it once (a free developer download). The app and `d3d12metal doctor` check for it.
- About 1 GB for Wine and the bottle, plus your games.
- To build from source: the Xcode Command Line Tools, `brew install meson ninja mingw-w64 directx-headers`, a DXC build; see [docs/DEVELOPING.md](docs/DEVELOPING.md).
  There are no binary releases yet.

## Install

```sh
git clone <this repository> && cd d3d12-metal
tools/build-dxilconv.sh                 # once: the DXBC converter (needs a DirectXShaderCompiler checkout, see DEVELOPING.md)
tools/build-wine.sh --dxc <path to dxc> # the layer: d3d12.dll, dxgi.dll, d3d12metal.so
tools/build-app.sh                      # build-app/D3D12 Metal.app (ad-hoc signed) with the CLI and the layer inside
open "build-app/D3D12 Metal.app"
```

The first start opens the **Setup Assistant**: it checks Rosetta, points you to Metal Shader Converter, downloads Wine (checksum verified), creates a bottle, installs Steam and
starts it so you can sign in. Install your games in Steam; they show up in the Library.

## Use

In the app: pick a game, press **Install layer** (done for you on **Play**), then **Play**. Settings on the game's page: performance HUD, GPU identity, feature level, DirectStorage
GPU decompression, logging, extra arguments and environment. **Report a Problem** writes a zip of logs with personal paths removed.

From a terminal (everything the app does; `--json` for scripts):

```sh
d3d12metal setup                 # Wine, bottle, Steam
d3d12metal doctor                # what is missing and how to fix it
d3d12metal steam start
d3d12metal library               # games found in the bottles
d3d12metal launch 1817070        # install the layer if needed, run first-launch setup, start the game
d3d12metal config 1817070 --hud on
d3d12metal logs 1817070
d3d12metal report 1817070        # zip for a bug report
d3d12metal uninstall 1817070     # restores every file it replaced
```

A game that is not in Steam: `d3d12metal game add --name "My Game" --folder <dir> --exe Game.exe`. More in [docs/USER_GUIDE.md](docs/USER_GUIDE.md).

## Tested games

| Game | Status |
|---|---|
| Marvel's Spider-Man Remastered | playable |
| Marvel's Spider-Man 2 | boots (first splash; work in progress) |
| Control (DX12) | boots |
| Godot 4 (D3D12 renderer), Microsoft D3D12 samples | works (test programs) |

Details and what each needs: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). Per-game settings live in [`profiles/`](profiles/) and are applied automatically.

## Troubleshooting

- **A game does not start, or says no GPU was found.** Run `d3d12metal doctor`. In the game's settings try GPU identity "AMD" or "NVIDIA".
- **"Rosetta does not advertise AVX2".** Use macOS 15 or newer. The app sets `ROSETTA_ADVERTISE_AVX=1` for Wine and the games itself.
- **Steam's window is black.** Use `Steam > Start` in the app (or `d3d12metal steam start`): it installs a small helper that makes Steam's browser draw under Wine.
- **First start takes minutes, later ones seconds.** The first run compiles the game's shaders; they are cached (`Preferences > Clear Shader Cache` resets them).
- **Logs.** The game page has **Open Logs**; `d3d12metal logs <game>`. For a bug report use **Report a Problem**.

## Documentation

[USER_GUIDE](docs/USER_GUIDE.md) · [COMPATIBILITY](docs/COMPATIBILITY.md) · [DEVELOPING](docs/DEVELOPING.md) · [ARCHITECTURE](docs/ARCHITECTURE.md) · [STATUS](docs/STATUS.md) ·
[LICENSING](docs/LICENSING.md) · [CONTRIBUTING](CONTRIBUTING.md)

## Prior art

- [DXMT](https://github.com/3Shain/dxmt): D3D11 to Metal
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton): D3D12 to Vulkan
- Apple's D3DMetal (Game Porting Toolkit)

## Legal

Licensed under the **LGPL-2.1-or-later** ([LICENSE](LICENSE)); third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and what may be redistributed in
[docs/LICENSING.md](docs/LICENSING.md). This project contains no game code or assets and works without modifying games. Never commit game binaries, assets, dumps or captures.
It is not affiliated with Apple, Microsoft, Valve or any game publisher; product names belong to their owners.
