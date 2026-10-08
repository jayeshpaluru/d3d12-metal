# Spider-Man Remastered: D3D12/DXGI requirements

Target: `Spider-Man.exe`, Steam app 1817070 (Nixxes, 2022). Compiled from public
sources (vkd3d-proton workarounds and issues, PCGamingWiki, ProtonDB, Apple
gaming reports). Items marked **[unverified]** must be confirmed with an API
trace of the game through our own DLLs (`D3D12METAL_LOG=1`).

## Must implement

- **SM 6.6 DXIL via the Agility SDK.** The exe exports `D3D12SDKVersion` and ships
  `D3D12Core`. Report SM 6.6 in `CheckFeatureSupport` and ignore the SDK path.
  There is no known DXBC. Which SM 6.6 features it uses (dynamic resources, wave
  ops, 16-bit, 64-bit atomics) is **[unverified]**; expect wave intrinsics,
  since it is a PS5 engine.
- **Placed resources and heap aliasing, without app initialization.**
  vkd3d-proton forces `FORCE_INITIAL_TRANSITION` for this exe
  (`libs/vkd3d/device_workarounds.c`). Put placed RTs and DSs in a defined
  state ourselves.
- **Huge typed buffer SRVs and UAVs.** The game makes views of an 800 MB buffer
  with up to 419,430,400 elements (R16_UINT, R16G16_SINT, RGBA8, RGB10A2). That
  is above D3D12's guaranteed 2^27 elements (vkd3d-proton #2071). Metal texture
  buffers have a texel limit, and Metal Shader Converter maps typed buffers to
  texture buffers, so we may need raw-buffer emulation or splitting.
- **VRAM budget.** The game runs its own residency manager on top of
  `IDXGIAdapter3::QueryVideoMemoryInfo` and `DedicatedVideoMemory`. A wrong value
  gives "not enough video memory" at launch.
- **Many PSOs created on loader threads.** PSO creation must be thread-safe, and
  Metal Shader Converter output should be cached to disk.
- **DXGI flip-model swapchain** for windowed and borderless modes. Exclusive
  fullscreen can be faked. The game needs Windows 10 1909 (build 18363) or later
  reported.
- **CPU:** the game expects AVX2 (Haswell minimum). Rosetta provides AVX2 on
  macOS 15 and later.
- **Launch:** no Denuvo or anti-cheat. Skip the launcher with `-nolauncher`.
  Do not set `SteamDeck=1`, because the game reads it.

## Should implement (after first render)

- **DXR 1.0 state objects** (DXIL libraries) for ray-traced reflections. Whether
  it also uses DXR 1.1 or RayQuery is **[unverified]**.
- **HDR output.** Whether it uses scRGB or HDR10 is **[unverified]**.
- **FSR 3.1, XeSS 1.3 and IGTI upscalers.** These are plain D3D12 work and the
  best path on Mac.
- **UPLOAD-heap reads by the GPU** are a streaming bottleneck (vkd3d
  `no_upload_hvv`). On unified memory, prefer shared storage.

## Can stub

NVAPI, DLSS and Reflex (report a non-NVIDIA adapter). AGS. D3D12 video
(cutscenes are Bink 2, decoded on the CPU). DirectStorage (not used,
**[unverified]**: check for `dstorage.dll`). Mesh shaders, sampler feedback and
VRS.

## Unknown, so assume the worst

Resource binding tier 3, root signature 1.1, async compute and copy queues,
ExecuteIndirect, tiled resources and enhanced barriers. Confirm with a trace.

## Reference performance (D3DMetal, not ours)

| Hardware | Software | Settings | Frame rate |
|---|---|---|---|
| M3 Max | CrossOver 24 + GPTK 2 | 4K High, RT High, FSR3 Performance + frame gen | 45–80 fps |
| M1 | CrossOver | 720p Low, FSR Quality | about 30 fps |
