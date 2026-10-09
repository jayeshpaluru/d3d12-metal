// SPDX-License-Identifier: LGPL-2.1-or-later
// The on-disk cache of Metal Shader Converter output (see docs/ARCHITECTURE.md, "Shader cache").
// Entries are files named after a SHA-256 key under a cache directory; every file carries a header
// and a checksum, so a damaged or foreign file is detected, deleted and rebuilt.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace mtlb {

// SHA-256 over whatever is fed to it.
class Hasher {
public:
    Hasher();
    void update(const void *data, size_t size);
    void update(const std::string &text) { update(text.data(), text.size()); }
    template <typename T>
    void update_value(const T &value) { update(&value, sizeof(value)); }
    std::array<uint8_t, 32> finish();

private:
    alignas(8) unsigned char state_[128];  // CC_SHA256_CTX
};

using CacheKey = std::array<uint8_t, 32>;

enum class CacheKind : uint32_t { Stage = 1, StageIn = 2 };

class DiskCache {
public:
    static DiskCache &instance();

    // Names the cache directory after the application (the first call wins; later calls and calls after the
    // first lookup are ignored).
    void configure(const std::string &app_name);

    bool enabled();

    // Reads an entry made of two byte strings. A missing entry returns false; a damaged one is deleted
    // (counted as corrupt) and also returns false.
    bool load(CacheKind kind, const CacheKey &key, std::vector<uint8_t> &first, std::string &second);
    // Deletes an entry the caller found unusable after loading it (the file was intact but its contents were rejected
    // by the converter or Metal); counted as corrupt, so the caller rebuilds and stores a new one.
    void discard(CacheKind kind, const CacheKey &key);
    // Writes an entry atomically (temporary file plus rename). Failures are ignored: the cache is optional.
    void store(CacheKind kind, const CacheKey &key, const void *first, size_t first_size, const std::string &second);

    // Identifies the converter build, so entries made by another version never match.
    static std::string converter_identity();

    struct Stats {
        std::atomic<uint64_t> hits{0}, misses{0}, writes{0}, corrupt{0}, evicted{0};
    };
    Stats &stats() { return stats_; }
    uint64_t bytes_on_disk();
    // Waits for a running eviction and runs one more, so the directory is within its limit (for tests and statistics).
    void settle();

private:
    DiskCache() = default;
    Stats stats_;
};

} // namespace mtlb
