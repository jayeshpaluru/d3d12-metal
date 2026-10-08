# Spider-Man 2: D3D12/DXGI requirements

Target: `Spider-Man2.exe`, Steam app 2651280 (Nixxes, January 2025, 140 GB).
This is compiled from public sources: the Steam page, Nixxes support, vkd3d-proton,
ProtonDB, CodeWeavers and Microsoft's DirectStorage blog. Items marked
**[unverified]** need a trace of the game running on our DLLs.

## Must implement

- **CPU: AVX2 and F16C.** Nixxes requires both. By default Rosetta advertises
  neither to x86 code. With `ROSETTA_ADVERTISE_AVX=1` it advertises AVX, AVX2,
  FMA, BMI2 and F16C (no AVX-512); this was checked on this machine.
  `tools/start-steam.sh` exports it, so games launched by Steam inherit it.
- **Shader model 6.6.** The game checks for it at startup ("Shader Model 6.6
  support not detected" under Parallels). We already report it. Whether it also
  checks for feature level 12_1 is **[unverified]**, so test 12_0 first.
- **DirectStorage 1.2.x with GPU GDeflate decompression.** The game ships
  `dstorage.dll` and `dstoragecore.dll`. DirectStorage chooses a path in this order:
  1. the driver's GDeflate metacommand
  2. its own fallback compute shader (needs SM 6.0, one compute queue and two
     copy queues)
  3. CPU decompression

  vkd3d-proton found the built-in fallback shader slow and fragile, so it always
  exposes the metacommand, backed by its own decompression shader.
  - Wine's `QueryIoRingCapabilities` returns `E_NOTIMPL`, so DirectStorage uses
    Win32 overlapped I/O.
  - Removing the DirectStorage DLLs is a working escape hatch. It fixed memory
    growth under Proton, but in Ratchet & Clank it cost texture quality.
- **Video memory budget.** `QueryVideoMemoryInfo` drives texture streaming.
- **Copy and compute queues with fences** that hold up under load.
- **A missing barrier in the game.** vkd3d-proton forces a compute barrier for
  shader hash `0x324071d329f05ccc`. Our backend already orders compute through
  barriers, so check whether that shader needs the same treatment.

## Should implement

- Carry over what Spider-Man Remastered needed: forced initialization of placed
  resources and typed views clamped to 2^28 elements.
- Watch memory: 32 GB is unified between the CPU and the GPU.

## Can stub

DXR (RT is optional: only the High RT tier and up use it), mesh shaders, VRS,
sampler feedback, enhanced barriers, IoRing, NVAPI, Reflex, AGS and the PSN
overlay (a PSN account is optional).

## Reference

- ProtonDB: Gold. Steam Deck Verified.
- CodeWeavers rates it "Limited Functionality" on CrossOver 26.
- An M4 base gets 40–50 fps at the lowest settings with frame generation.
- We expect roughly 30–45 fps on the M2 Max at a 1080p-class internal
  resolution with upscaling. That is an estimate.
