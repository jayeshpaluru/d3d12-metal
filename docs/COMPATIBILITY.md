# Compatibility

Tested on an M2 Max (32 GB), macOS 27, Wine 11.18 (Gcenx build, x86-64 under Rosetta 2), Apple Metal Shader Converter 3.x.
Status words: **playable** (reached gameplay and it is usable), **boots** (starts and renders something, not playable yet), **untested**.
Profiles live in [`profiles/`](../profiles); a game without one gets the generic profile.

| Game | Steam app | Status | Notes |
|---|---|---|---|
| Marvel's Spider-Man Remastered | 1817070 | playable | Open world about 55-60 fps at 1080p-class settings; first start compiles ~7800 pipelines (minutes); started through Steam with `-nolauncher`. FSR 3.1 and XeSS available, DLSS and frame generation are not |
| Marvel's Spider-Man 2 | 2651280 | boots | Needs AVX2/F16C (`ROSETTA_ADVERTISE_AVX=1`, set for you); installs the PlayStation PC SDK runtime on first launch; DirectStorage GPU GDeflate is provided by the layer. Renders the first splash, does not get past it yet |
| Control (DX12) | 870780 | boots | Start `Control_DX12.exe` directly; uses the game folder's own `avifil32.dll` |
| Godot 4.7 (D3D12 renderer) | - | works | Forward+ renders correctly (`tools/run-godot-test.sh`); the Mobile renderer does not (view instancing) |
| Microsoft D3D12 samples (HelloTriangle, textures, ...) | - | works | `tests/wine`, `tools/run-wine-tests.sh` |

## What the layer does not do (yet)

- Ray tracing, mesh shaders, variable rate shading, enhanced barriers, tiled resources, stream output, HDR, exclusive fullscreen: absent or accepted and ignored.
  Games that require them will not run. Feature level is 12_0 (12_1 with ROVs by setting; conservative rasterization is unsupported).
- DLSS, frame generation and vendor libraries (NVAPI, AGS) are not available; the game's FSR/XeSS paths can work.
- Anti-cheat that needs a Windows kernel driver does not run under Wine.
- Windowed only: the swap chain always scales the image to the window.

See [STATUS.md](STATUS.md) for the full list.

## Reporting a result

Run `alloy report <game>` (or **Report a Problem** in the app), and open an issue with the zip, the game, your Mac model and macOS version. A working
profile is the best contribution: [DEVELOPING.md](DEVELOPING.md#adding-a-game).
