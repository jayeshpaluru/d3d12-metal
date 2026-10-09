// SPDX-License-Identifier: LGPL-2.1-or-later
// On-disk cache of converted shaders. Layout:
//   <dir>/<first two hex digits of the key>/<key as hex>.d3mc
// File: Header, then the first string, then the second string. The header repeats the key and holds the
// payload's SHA-256 (truncated), so truncation, bit rot and files written by another version are rejected.
// Writes go to a temporary file of the same directory and are renamed into place, so readers (other
// threads, other processes) never see a partial file. The directory is trimmed to a size limit by
// deleting the least recently used files (a hit refreshes the file's modification time).
#include "disk_cache.h"

#include "bridge/mtlb.h"

#include <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <metal_irconverter/metal_irconverter.h>

namespace mtlb {

namespace {

constexpr char kMagic[8] = {'D', '3', 'D', 'M', 'C', 'A', 'C', '1'};
constexpr uint32_t kFormatVersion = 1;
constexpr uint64_t kDefaultMaxBytes = 1024ull << 20;

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t kind;
    uint8_t key[32];
    uint64_t first_size;
    uint64_t second_size;
    uint8_t payload_hash[16];
};

struct State {
    std::mutex mutex;  // configuration (before the first use) only
    std::string app_name;
    std::once_flag resolve_once;
    // Written once by resolve(); read without a lock afterwards (std::call_once orders them).
    bool is_enabled = false;
    std::string dir;
    uint64_t max_bytes = kDefaultMaxBytes;
    // Eviction runs on a thread of its own, one at a time.
    std::atomic<bool> scanned{false};
    std::atomic<bool> evicting{false};
    std::atomic<int64_t> last_eviction_ms{0};
    std::mutex evict_mutex;  // pairs with evict_done for waiters
    std::condition_variable evict_done;
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> sequence{0};
};

// Never destroyed: an eviction thread may still be running at process exit.
State &state()
{
    static State &s = *new State;
    return s;
}

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string hex(const CacheKey &key)
{
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (uint8_t b : key) {
        text.push_back(digits[b >> 4]);
        text.push_back(digits[b & 15]);
    }
    return text;
}

bool make_directories(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string prefix = path.substr(0, i);
            if (mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
    }
    return true;
}

// Resolves the cache directory (once): D3D12METAL_CACHE=0 disables, D3D12METAL_CACHE_DIR replaces the
// default ~/Library/Caches/d3d12metal/<application>.
void resolve(State &s)
{
    std::lock_guard<std::mutex> lock(s.mutex);  // against configure()
    const char *on = std::getenv("D3D12METAL_CACHE");
    if (on && std::string(on) == "0")
        return;
    if (const char *limit = std::getenv("D3D12METAL_CACHE_MAX_MB")) {
        const long long mb = std::atoll(limit);
        if (mb > 0)
            s.max_bytes = static_cast<uint64_t>(mb) << 20;
    }
    if (const char *dir = std::getenv("D3D12METAL_CACHE_DIR"); dir && *dir) {
        s.dir = dir;
    } else {
        const char *home = std::getenv("HOME");
        if (!home || !*home) {
            const passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : nullptr;
        }
        if (!home)
            return;
        std::string app = s.app_name.empty() ? "default" : s.app_name;
        for (char &c : app)
            if (c == '/' || c == '\\' || c == ':')
                c = '_';
        s.dir = std::string(home) + "/Library/Caches/d3d12metal/" + app;
    }
    s.is_enabled = make_directories(s.dir);
}

std::string path_of(const State &s, const CacheKey &key)
{
    const std::string name = hex(key);
    return s.dir + "/" + name.substr(0, 2) + "/" + name + ".d3mc";
}

void payload_hash(const void *first, size_t first_size, const std::string &second, uint8_t out[16])
{
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    CC_SHA256_Update(&ctx, first, static_cast<CC_LONG>(first_size));
    CC_SHA256_Update(&ctx, second.data(), static_cast<CC_LONG>(second.size()));
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    std::memcpy(out, digest, 16);
}

struct FileInfo {
    std::string path;
    uint64_t size;
    time_t mtime;
};

// Lists every entry file (and stale temporary files) under the cache directory.
std::vector<FileInfo> list_files(const std::string &dir)
{
    std::vector<FileInfo> files;
    DIR *top = opendir(dir.c_str());
    if (!top)
        return files;
    while (const dirent *shard = readdir(top)) {
        if (shard->d_name[0] == '.')
            continue;
        const std::string shard_path = dir + "/" + shard->d_name;
        DIR *d = opendir(shard_path.c_str());
        if (!d)
            continue;
        while (const dirent *e = readdir(d)) {
            if (e->d_name[0] == '.' && std::strncmp(e->d_name, ".tmp-", 5) != 0)
                continue;
            const std::string path = shard_path + "/" + e->d_name;
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                files.push_back({path, static_cast<uint64_t>(st.st_size), st.st_mtime});
        }
        closedir(d);
    }
    closedir(top);
    return files;
}

// Deletes the oldest files until the directory is at 80% of its limit. Works from a snapshot of the directory
// listing and holds no lock the cache's readers take.
void evict(State &s, DiskCache::Stats &stats)
{
    std::vector<FileInfo> files = list_files(s.dir);
    uint64_t total = 0;
    for (const FileInfo &f : files)
        total += f.size;
    const uint64_t target = s.max_bytes / 10 * 8;
    if (total > target) {
        std::sort(files.begin(), files.end(), [](const FileInfo &a, const FileInfo &b) { return a.mtime < b.mtime; });
        for (const FileInfo &f : files) {
            if (total <= target)
                break;
            if (unlink(f.path.c_str()) == 0) {
                total -= f.size;
                stats.evicted++;
            }
        }
    }
    s.total_bytes = total;
}

// Starts an eviction pass on a thread of its own unless one is running (or one finished a moment ago and the
// directory is not far over its limit).
void schedule_eviction(State &s, DiskCache::Stats &stats, bool force)
{
    const bool far_over = s.total_bytes.load() > s.max_bytes + s.max_bytes / 4;
    if (!force && !far_over && now_ms() - s.last_eviction_ms.load() < 5000)
        return;
    if (s.evicting.exchange(true))
        return;
    std::thread([&s, &stats] {
        evict(s, stats);
        s.last_eviction_ms = now_ms();
        {
            std::lock_guard<std::mutex> lock(s.evict_mutex);
            s.evicting = false;
        }
        s.evict_done.notify_all();
    }).detach();
}

} // namespace

// ---- Hasher ---------------------------------------------------------------------------------------

static_assert(sizeof(CC_SHA256_CTX) <= 128, "Hasher state is too small");

Hasher::Hasher()
{
    CC_SHA256_Init(reinterpret_cast<CC_SHA256_CTX *>(state_));
}

void Hasher::update(const void *data, size_t size)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    while (size) {  // CC_LONG is 32 bits
        const size_t n = std::min<size_t>(size, 1u << 30);
        CC_SHA256_Update(reinterpret_cast<CC_SHA256_CTX *>(state_), bytes, static_cast<CC_LONG>(n));
        bytes += n;
        size -= n;
    }
}

std::array<uint8_t, 32> Hasher::finish()
{
    std::array<uint8_t, 32> digest;
    CC_SHA256_Final(digest.data(), reinterpret_cast<CC_SHA256_CTX *>(state_));
    return digest;
}

// ---- DiskCache ------------------------------------------------------------------------------------

DiskCache &DiskCache::instance()
{
    static DiskCache cache;
    return cache;
}

void DiskCache::configure(const std::string &app_name)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.app_name.empty())
        s.app_name = app_name;  // only matters before the first lookup resolved the directory
}

bool DiskCache::enabled()
{
    State &s = state();
    std::call_once(s.resolve_once, [&s] { resolve(s); });
    return s.is_enabled;
}

std::string DiskCache::converter_identity()
{
    // The converter is a dynamic library: its path, size and modification time tell versions apart.
    static const std::string identity = [] {
        Dl_info info;
        std::string text = "metalirconverter";
        if (dladdr(reinterpret_cast<void *>(&IRCompilerCreate), &info) && info.dli_fname) {
            struct stat st;
            text += std::string(":") + info.dli_fname;
            if (stat(info.dli_fname, &st) == 0)
                text += ":" + std::to_string(st.st_size) + ":" + std::to_string(st.st_mtime);
        }
        return text;
    }();
    return identity;
}

bool DiskCache::load(CacheKind kind, const CacheKey &key, std::vector<uint8_t> &first, std::string &second)
{
    State &s = state();
    if (!enabled())
        return false;
    const std::string path = path_of(s, key);
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        stats_.misses++;
        return false;
    }
    bool ok = false;
    struct stat st;
    Header header;
    if (fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) >= sizeof(header)
        && pread(fd, &header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header))
        && std::memcmp(header.magic, kMagic, sizeof(kMagic)) == 0 && header.version == kFormatVersion
        && header.kind == static_cast<uint32_t>(kind) && std::memcmp(header.key, key.data(), 32) == 0
        && header.first_size <= static_cast<uint64_t>(st.st_size) && header.second_size <= static_cast<uint64_t>(st.st_size)
        && sizeof(header) + header.first_size + header.second_size == static_cast<uint64_t>(st.st_size)) {
        first.resize(header.first_size);
        second.resize(header.second_size);
        const bool read_ok =
            pread(fd, first.data(), first.size(), sizeof(header)) == static_cast<ssize_t>(first.size())
            && pread(fd, second.data(), second.size(), sizeof(header) + first.size()) == static_cast<ssize_t>(second.size());
        uint8_t hash[16];
        if (read_ok) {
            payload_hash(first.data(), first.size(), second, hash);
            ok = std::memcmp(hash, header.payload_hash, 16) == 0;
        }
    }
    close(fd);
    if (!ok) {
        // Damaged, foreign or from another version: forget it, the caller rebuilds and stores a new one.
        unlink(path.c_str());
        stats_.corrupt++;
        return false;
    }
    utimes(path.c_str(), nullptr);  // most recently used
    stats_.hits++;
    return true;
}

void DiskCache::discard(CacheKind, const CacheKey &key)
{
    State &s = state();
    if (!enabled())
        return;
    unlink(path_of(s, key).c_str());
    stats_.corrupt++;
}

void DiskCache::store(CacheKind kind, const CacheKey &key, const void *first, size_t first_size, const std::string &second)
{
    State &s = state();
    if (!enabled())
        return;
    Header header = {};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kFormatVersion;
    header.kind = static_cast<uint32_t>(kind);
    std::memcpy(header.key, key.data(), 32);
    header.first_size = first_size;
    header.second_size = second.size();
    payload_hash(first, first_size, second, header.payload_hash);

    const std::string path = path_of(s, key);
    const std::string shard = path.substr(0, path.rfind('/'));
    if (!make_directories(shard))
        return;
    const std::string temp = shard + "/.tmp-" + std::to_string(getpid()) + "-" + std::to_string(s.sequence++);
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return;
    auto write_all = [&](const void *data, size_t size) {
        const auto *p = static_cast<const uint8_t *>(data);
        while (size) {
            const ssize_t n = write(fd, p, size);
            if (n <= 0)
                return false;
            p += n;
            size -= static_cast<size_t>(n);
        }
        return true;
    };
    const bool ok = write_all(&header, sizeof(header)) && write_all(first, first_size) && write_all(second.data(), second.size());
    close(fd);
    if (!ok || rename(temp.c_str(), path.c_str()) != 0) {
        unlink(temp.c_str());
        return;
    }
    stats_.writes++;

    const uint64_t size = sizeof(header) + first_size + second.size();
    // The first store measures the directory (an earlier run may have left it too big); after that the running total
    // decides. Both happen on the eviction thread.
    if (!s.scanned.exchange(true))
        schedule_eviction(s, stats_, true);
    else if (s.total_bytes.fetch_add(size) + size > s.max_bytes)
        schedule_eviction(s, stats_, false);
}

void DiskCache::settle()
{
    State &s = state();
    if (!enabled())
        return;
    std::unique_lock<std::mutex> lock(s.evict_mutex);
    s.evict_done.wait(lock, [&s] { return !s.evicting.load(); });
    lock.unlock();
    schedule_eviction(s, stats_, true);  // the running total is approximate: measure again
    lock.lock();
    s.evict_done.wait(lock, [&s] { return !s.evicting.load(); });
}

uint64_t DiskCache::bytes_on_disk()
{
    State &s = state();
    if (!enabled())
        return 0;
    uint64_t total = 0;
    for (const FileInfo &f : list_files(s.dir))
        total += f.size;
    return total;
}

} // namespace mtlb

extern "C" void mtlb_cache_configure(const char *app_name)
{
    if (app_name)
        mtlb::DiskCache::instance().configure(app_name);
}

extern "C" void mtlb_cache_get_stats(mtlb_cache_stats *out)
{
    if (!out)
        return;
    mtlb::DiskCache &cache = mtlb::DiskCache::instance();
    cache.settle();
    const mtlb::DiskCache::Stats &s = cache.stats();
    *out = {s.hits, s.misses, s.writes, s.corrupt, s.evicted, cache.bytes_on_disk()};
}
