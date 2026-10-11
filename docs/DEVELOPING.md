# Developing d3d12-metal

How the repository is organised, how to build and test every part, and how to add a game.
The user-facing documents are [README.md](../README.md) and [USER_GUIDE.md](USER_GUIDE.md); the design is in
[ARCHITECTURE.md](ARCHITECTURE.md), the state of the layer in [STATUS.md](STATUS.md).

## Layout

| Path | What |
|---|---|
| `src/` | the layer: `d3d12/` and `dxgi/` front-end, `bridge/` (Metal backend and the Wine PE/unix transport), `common/`, `pe/` (the exports of the DLLs) |
| `tests/` | native tests (arm64, headless Metal), portable tests that also build as Win32 programs, `tests/wine` Win32 samples, `tests/godot` |
| `app/` | the Swift package: `Core` library, `d3d12metal` CLI, `D3D12MetalApp` (SwiftUI), tests |
| `profiles/` | per-game profiles (JSON): launch options, conf keys, first-launch setup, notes |
| `tools/` | build, test and dev scripts; `build-app.sh` makes the app bundle, `check.sh` runs everything |
| `docs/` | documentation |

## One command

```sh
tools/check.sh                 # personal-data grep, SPDX headers, profiles, native build + tests, swift build + swift test
tools/check.sh --validation --app   # also the Metal API validation run and the app bundle
```

## Prerequisites

- macOS on Apple Silicon (arm64) with the Xcode Command Line Tools
- [Metal Shader Converter](https://developer.apple.com/metal/shader-converter/) (headers in `/usr/local/include`, `libmetalirconverter.dylib` in `/usr/local/lib`)
- `brew install meson ninja directx-headers` (DirectX-Headers provides `d3d12.h`)
- A DXC build (HLSL to DXIL) for the test shaders; found on `PATH`, or pass it explicitly, e.g. `meson setup build -Ddxc=$DEPS_DIR/dxc-build/bin/dxc`
- For the Wine build: `brew install mingw-w64` (the PE DLLs and Win32 test programs), a Wine 11 for macOS (x86-64, runs under Rosetta 2; developed against the Gcenx build 11.18), Rosetta 2, and a Metal Shader Converter dylib that includes the x86-64 slice (the shipped one is universal)

## Building and testing

```sh
meson setup build -Ddxc=$DEPS_DIR/dxc-build/bin/dxc   # omit if dxc is on PATH
meson compile -C build
meson test -C build
```

### Wine build

The Wine flavour is two cross builds, `d3d12.dll` + `dxgi.dll` (x86-64 PE, MinGW) and `x86_64-unix/d3d12metal.so` (x86-64 macOS, loaded by Wine's unix loader), plus the Win32 test programs:

```sh
tools/build-wine.sh --dxc $DEPS_DIR/dxc-build/bin/dxc    # -> build-wine/out/
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
mkdir -p $DEPS_DIR/godot && cd $DEPS_DIR/godot
gh release download 4.7.2-stable -R godotengine/godot -p 'Godot_v4.7.2-stable_win64.exe.zip' -D . && unzip Godot_v4.7.2-stable_win64.exe.zip
cd - && tools/build-wine.sh --dxc <dxc> && tools/run-godot-test.sh [--method forward_plus|mobile] [--frames N] [--stats]
```

The runner copies the DLLs next to Godot, runs `--rendering-driver d3d12`, validates the screenshot
(`tools/check_godot_screenshot.py`) and writes `build-wine/screens/godot-<method>.png` and `build-wine/logs/godot-<method>.log`.
Forward+ passes; Mobile does not render yet (see docs/STATUS.md).

#### Options

Every option is an environment variable `D3D12METAL_<KEY>` or a `key=value` line in `d3d12metal.conf` next to `d3d12.dll` (read once at
startup; `#` starts a comment; `D3D12METAL_CONF=<file>` names another file). The environment wins. The file is how a game started by
Steam is configured, since it does not see the environment of your shell.

| Key (`D3D12METAL_<KEY>`) | Effect |
|---|---|
| `LOG=1` | trace every bridge call (both sides; very verbose) |
| `LOG_FILE=<unix path>` | append the layer's messages (PE and unix side, whole lines) to this file as well as stderr |
| `TRACE=1` | API trace: every D3D12/DXGI method the application calls, with arguments and result, the first 50 calls of each method and then every 1000th; the distinct methods and their exact call counts are written to `<LOG_FILE>.methods` (at exit and every 5 s) |
| `STATS=1` | per-frame counters every 120 presents (submits, encoders, barriers, syncs, descriptor writes, PSO time, unix calls) |
| `CACHE_DIR`, `CACHE=0`, `CACHE_MAX_MB` | shader disk cache location, off switch, size limit (default `~/Library/Caches/d3d12metal/<exe>`, 1024 MB) |
| `DUMP_FAILED=<dir>` | keep DXIL the shader converter rejects (inspect with `dxc -dumpbin`) |
| `DUMP_PRESENT=<png>`, `DUMP_PRESENT_FRAME=N` | write the Nth presented frame |
| `NO_BARRIERS=1` | turn barrier synchronisation off (to show a test needs it) |
| `HUD=1` | performance overlay: FPS, frame time and GPU time per frame, top-left, drawn in the present pass |
| `ADAPTER_NAME`, `VENDOR_ID`, `DEVICE_ID` | the identity the adapter reports (hex ids), for games that whitelist GPUs |
| `FEATURE_LEVEL=12_1` | report feature level 12_1 (with ROVs) instead of 12_0 |
| `GDEFLATE=0` | hide the GPU GDeflate meta command, so DirectStorage uses its own decompression |
| `QUIRK_FORCE_COMPUTE_BARRIER=<hex hashes>` | shader hashes that get a UAV barrier after every dispatch (per-game, from the profile) |
| `SHADER_HASHES=1` | log the hash of every shader a pipeline is created with |

If DirectX-Headers is not installed under `/opt/homebrew` and `pkg-config` is unavailable, pass `-Ddirectx_headers_prefix=<prefix>` to `meson setup`.

The tests are headless: they render offscreen on the default Metal device and read the pixels back.

## Running the game

Marvel's Spider-Man Remastered (Steam app 1817070) runs in the Steam prefix (`$DEPS_DIR/wineprefix-steam`, Steam started with
`tools/start-steam.sh`). After `tools/build-wine.sh`:

```sh
tools/install-game.sh            # d3d12.dll, dxgi.dll, x86_64-unix/d3d12metal.so, d3d12metal.conf next to Spider-Man.exe + registry overrides
tools/run-game.sh                # install, launch through the running Steam, watch logs, take screenshots
tools/run-game.sh --kill         # stop just the game (never wineserver)
tools/uninstall-game.sh          # restore backups, remove the overrides
```

- `install-game.sh` finds `steamapps/common/*/Spider-Man.exe` (or `GAME_DIR=<folder>`), renames files it would overwrite to `*.d3d12metal-orig`,
  leaves the game's `D3D12/` folder alone (the layer ignores the Agility SDK path), writes `d3d12metal.conf` once (log file
  `build-wine/game-logs/d3d12metal.log`, `trace=1`, `stats=1`; edit it freely) and sets `d3d12`, `d3d12core`, `dxgi` = `native,builtin` under
  `HKCU\Software\Wine\AppDefaults\Spider-Man.exe\DllOverrides` with `wine reg add`, so the overrides apply when Steam starts the game.
  Both install and uninstall are idempotent and take `--dry-run`.
- `run-game.sh` refuses to start unless Steam is already running, launches `steam.exe -applaunch 1817070 -nolauncher [args]`, then until the
  game exits (or `--timeout`, default 30 min) prints the layer log and any `*.log`/`*.txt`/`*.dmp` the game writes under the prefix's user profile or its
  folder, and captures the game window every 15 s into `build-wine/game-screens/` (macOS needs Screen Recording permission for the terminal and an
  awake, unlocked display). The previous layer log is kept as `d3d12metal.log.prev`.
- The inventory of what the game needs is `build-wine/game-logs/d3d12metal.log.methods` (every distinct D3D12/DXGI method with its call count, in
  order of first call) next to the trace itself. Grep the log for `not implemented` and `failed`.
- `run-game.sh --direct` runs an exe straight under wine in `GAME_WINEPREFIX` (default: the test prefix) with `GAME_DIR` set, without Steam. The harness is
  checked with a stand-in: a copy of `hello_triangle.exe` named `Spider-Man.exe` in a fake game folder.


## The Swift package (`app/`)

`Core` (Foundation only, plus Metal for the GPU line of `doctor`) holds everything: Steam library parsing, the PE import scanner, the
profile database, bottles, the Wine runtime download, the layer installer, launching, diagnostics. The CLI and the app are thin shells over it.

```sh
cd app && swift build            # debug build of everything
swift test                       # Core tests (Swift Testing)
.build/debug/d3d12metal --help
cd .. && tools/build-app.sh      # build-app/<name>.app, ad-hoc signed, with the CLI, layer, profiles and licences inside
```

Notes for a machine with only the Command Line Tools (no `xcodebuild`):

- SwiftUI's `@State` and friends are macros whose plug-in ships with Xcode. `app/Package.swift` adds `-plugin-path` to Xcode's copy when
  `/Applications/Xcode.app` exists (Xcode need not be selected or licensed). Without any Xcode the app target cannot compile.
- `swift test` needs the rpath to the Command Line Tools' `Testing.framework`, also set in `Package.swift`. The Swift Testing macro plug-in is
  occasionally "not found" on a busy machine; running the tests again works (`tools/check.sh` retries).

Where things are found (first match): the layer build is `config.layerDir`, `D3D12METAL_LAYER_DIR`, the bundle's `Resources/layer`, the managed copy,
then `build-wine/out` of a source checkout (found by walking up from the current directory or the executable; `D3D12METAL_REPO` forces it).
Profiles: the bundle, `D3D12METAL_PROFILES`, the checkout's `profiles/`, then `~/Library/Application Support/D3D12Metal/profiles/` (which wins by id).
All state lives in `D3D12METAL_HOME` (default `~/Library/Application Support/D3D12Metal`); use another one to test without touching your setup:

```sh
export D3D12METAL_HOME=$PWD/build/test-home
app/.build/debug/d3d12metal bottle create Test && app/.build/debug/d3d12metal game add --name Hello --folder <dir> --exe hello_triangle.exe --bottle Test
```

App start-up hooks for screenshots and scripts: `D3D12METAL_UI_SECTION=library|steam|bottles|health`, `D3D12METAL_UI_GAME=<game id>`,
`D3D12METAL_UI_SETUP=0|1`.

## Adding a game

1. Install it in a bottle (Steam, or any folder) and run `d3d12metal library`; a Steam game appears when its `appmanifest` says it is installed and an exe in
   its folder links `d3d12.dll` (by name or ordinal; or mentions `D3D12CreateDevice`). Others: `d3d12metal game add`.
2. `d3d12metal install <game>`, `d3d12metal config <game> --log trace`, `d3d12metal launch <game>`, read `d3d12metal logs <game>`.
3. Write `profiles/<name>.json` once you know what the game needs (schema below) and add the row to [COMPATIBILITY.md](COMPATIBILITY.md).
   Anything game-specific belongs in a profile, never in `src/`.

```json
{
  "id": "my-game", "name": "My Game", "status": "untested|boots|playable",
  "match": { "steamAppId": 123, "exe": ["MyGame.exe"] },
  "exe": "MyGame.exe", "installDir": "My Game",
  "launch": { "mode": "steam|direct", "args": ["-nolauncher"], "env": { "ROSETTA_ADVERTISE_AVX": "1" }, "windowTitle": "My Game" },
  "conf": { "feature_level": "12_1", "vendor_id": "1002" },
  "dllOverrides": { "avifil32": "native,builtin" },
  "requirements": [ { "id": "runtime", "title": "...", "doneKey": "HKCU\\SOFTWARE\\Vendor\\Game", "run": ["setup.bat", "quiet"] } ],
  "notes": ["shown in the app"]
}
```

`conf` keys are the lower-case option names of the table above. `requirements` run once before the first launch (the Windows equivalent of Steam's
`installscript.vdf`, which Steam does not run under Wine); `doneKey` is a registry key whose presence means done.

## The old shell tools

`tools/install-game.sh`, `uninstall-game.sh`, `run-game.sh`, `start-steam.sh` and `tools/game/*` are the development tools the CLI grew out of
(they drive a Steam prefix by environment variables, with `GAME=sm1|sm2`). They keep working and use the same manifest format as the CLI, so
either can undo the other's install. Paths come from the environment (`DEPS_DIR`, `WINE_ROOT`, `STEAM_WINEPREFIX`, `GAME_WINEPREFIX`).

## Tests, in short

- `meson test -C build` (native, ~55 tests), `--setup validation` (Metal API validation), `tools/run-wine-tests.sh` (every portable test and the samples under Wine),
  `tools/run-godot-test.sh`, `swift test` in `app/`.
- The Wine tests use a prefix of their own (`WINEPREFIX`, default `$DEPS_DIR/wineprefix`), never your Steam prefix.
- The overlay is tested natively (`test_hud_on`, `test_hud_off`: pixels of the present dump) and by eye under Wine with `hello_triangle`.
