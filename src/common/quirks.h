// Workarounds for individual shaders (and games), applied when a pipeline is created.
//
// Some games have shaders that are only correct with the order of execution one GPU gives them; other
// implementations (vkd3d-proton keeps such a list) force a behaviour for them. A quirk is a bit of `Quirk`
// attached to a shader by its hash, from two places:
//   * the table in quirks.cpp (shaders known to need one, with the executable they belong to), and
//   * d3d12metal.conf / the environment, one key per quirk: `quirk_force_compute_barrier=0x324071d329f05ccc,...`
//     (D3D12METAL_QUIRK_FORCE_COMPUTE_BARRIER). A value lists hashes in hex, separated by commas or spaces.
//
// The shader hash is the one vkd3d-proton uses, so its hashes (see libs/vkd3d/device_workarounds.c there) work
// here as they are: a 64-bit FNV-1 hash (multiply, then xor) over the bytecode exactly as the application
// passed it to CreateGraphicsPipelineState / CreateComputePipelineState (the DXBC container). With
// D3D12METAL_SHADER_HASHES=1 (or `shader_hashes=1`) every pipeline creation logs the hashes of its shaders and the
// quirks that apply, which is how to identify a shader in a trace.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace d3d12m {

enum Quirk : uint32_t {
    // A full barrier (UAV barrier on all resources) after every dispatch of the shader: the game misses one between
    // this shader and the work that reads its results.
    kQuirkForceComputeBarrier = 1u << 0,
};

// FNV-1 over the bytes (vkd3d-proton's vkd3d_shader_hash).
uint64_t shader_hash(const void *code, size_t size);

// An entry of the built-in table: the quirks for the shader `hash` of the executable `exe` (without extension,
// compared without regard to case; empty: any).
struct QuirkEntry {
    uint64_t hash;
    const char *exe;
    uint32_t quirks;
};

// The quirks of a shader of the given hash in the running executable: the built-in table plus the configuration.
uint32_t quirks_for_shader(uint64_t hash);

// The pure parts: the quirks of `hash` in `exe` according to `table` and the hash lists of the configuration
// values (`force_compute_barrier` is the text of `quirk_force_compute_barrier`, may be null).
uint32_t lookup_quirks(const QuirkEntry *table, size_t count, const std::string &exe, uint64_t hash,
                       const char *force_compute_barrier);
// Parses a list of hexadecimal hashes ("0x1234abcd, 5678"); false if a part is not a hash.
bool parse_hash_list(const char *text, std::string *error, void (*add)(uint64_t, void *), void *user);

// Logs the hashes of a pipeline's shaders and quirks when the shader_hashes option is on; `stage` names them
// ("cs", "vs", ...).
void log_shader_hash(const char *stage, uint64_t hash, uint32_t quirks);
bool shader_hashes_logged();

} // namespace d3d12m
