// SPDX-License-Identifier: LGPL-2.1-or-later
#include "common/quirks.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>

#include "common/config.h"
#include "common/export.h"
#include "common/log.h"
#include "common/platform.h"

namespace d3d12m {

namespace {

// Shaders known to need a workaround.
const QuirkEntry kBuiltinQuirks[] = {
    // Marvel's Spider-Man 2: a compute shader whose results are read by the next dispatch without a barrier (vkd3d-proton
    // applies VKD3D_SHADER_QUIRK_FORCE_COMPUTE_BARRIER to it). [unverified here: the hash is vkd3d's; confirm with a
    // trace of the game that a shader of this hash is created]
    {0x324071d329f05ccc, "Spider-Man2", kQuirkForceComputeBarrier},
};

bool equal_nocase(const std::string &a, const char *b)
{
    const size_t n = std::strlen(b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

void add_to_set(uint64_t hash, void *user)
{
    static_cast<std::set<uint64_t> *>(user)->insert(hash);
}

struct Configured {
    std::set<uint64_t> force_compute_barrier;
    std::string exe;

    Configured()
    {
        exe = platform_executable_name();
        if (const char *value = config_get("QUIRK_FORCE_COMPUTE_BARRIER")) {
            std::string error;
            if (!parse_hash_list(value, &error, add_to_set, &force_compute_barrier))
                D3D12M_LOG("quirk_force_compute_barrier: %s", error.c_str());
        }
    }
};

const Configured &configured()
{
    static const Configured c;
    return c;
}

} // namespace

uint64_t shader_hash(const void *code, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *bytes = static_cast<const uint8_t *>(code);
    for (size_t i = 0; i < size; ++i)
        h = (h * 0x100000001b3ull) ^ bytes[i];
    return h;
}

bool parse_hash_list(const char *text, std::string *error, void (*add)(uint64_t, void *), void *user)
{
    const char *p = text;
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ','))
            ++p;
        if (!*p)
            break;
        char *end = nullptr;
        const unsigned long long value = std::strtoull(p, &end, 16);
        if (end == p || (*end && !std::isspace(static_cast<unsigned char>(*end)) && *end != ',')) {
            if (error)
                *error = std::string("not a hexadecimal shader hash: \"") + p + "\"";
            return false;
        }
        add(value, user);
        p = end;
    }
    return true;
}

uint32_t lookup_quirks(const QuirkEntry *table, size_t count, const std::string &exe, uint64_t hash,
                       const char *force_compute_barrier)
{
    uint32_t quirks = 0;
    for (size_t i = 0; i < count; ++i) {
        if (table[i].hash == hash && (!table[i].exe || !*table[i].exe || equal_nocase(exe, table[i].exe)))
            quirks |= table[i].quirks;
    }
    if (force_compute_barrier) {
        std::set<uint64_t> hashes;
        if (parse_hash_list(force_compute_barrier, nullptr, add_to_set, &hashes) && hashes.count(hash))
            quirks |= kQuirkForceComputeBarrier;
    }
    return quirks;
}

uint32_t quirks_for_shader(uint64_t hash)
{
    const Configured &c = configured();
    uint32_t quirks = lookup_quirks(kBuiltinQuirks, sizeof(kBuiltinQuirks) / sizeof(kBuiltinQuirks[0]), c.exe, hash, nullptr);
    if (c.force_compute_barrier.count(hash))
        quirks |= kQuirkForceComputeBarrier;
    return quirks;
}

// For the tests (the library hides everything else).
D3D12M_EXPORT uint64_t d3d12m_shader_hash(const void *code, size_t size)
{
    return shader_hash(code, size);
}

// The quirks of `hash` in an executable named `exe`: the built-in table and the hash list `force_compute_barrier`.
D3D12M_EXPORT uint32_t d3d12m_lookup_quirks(const char *exe, uint64_t hash, const char *force_compute_barrier)
{
    return lookup_quirks(kBuiltinQuirks, sizeof(kBuiltinQuirks) / sizeof(kBuiltinQuirks[0]), exe, hash, force_compute_barrier);
}

bool shader_hashes_logged()
{
    static const bool on = config_flag("SHADER_HASHES");
    return on;
}

void log_shader_hash(const char *stage, uint64_t hash, uint32_t quirks)
{
    if (shader_hashes_logged() || quirks)
        D3D12M_LOG("shader %s hash %016llx%s", stage, static_cast<unsigned long long>(hash),
                   quirks ? (quirks & kQuirkForceComputeBarrier ? " (quirk: force compute barrier)" : " (quirk)") : "");
}

} // namespace d3d12m
