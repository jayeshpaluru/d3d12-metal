// Front-end counters for D3D12METAL_STATS=1: when the variable is set, every 120th Present prints the
// per-frame averages of what happened since the last report (submits, encoders, descriptor writes, ...)
// on stderr. When it is not set, the counters cost one predictable branch.
#pragma once

#include <atomic>
#include <cstdint>

namespace d3d12m {

enum class Stat : unsigned {
    DescriptorWrites,  // CreateXxxView, CreateSampler, descriptors copied
    PsoCreations,
    Submits,           // ExecuteCommandLists calls
    CommandLists,      // lists executed
    StreamBytes,       // command stream bytes handed to the backend
    PsoNanos,          // time spent in pipeline creation (all threads)
    PsoTotal,          // pipelines created since the start (never reset by a report)
    Count
};

extern bool g_stats_enabled;
extern std::atomic<uint64_t> g_stat_values[static_cast<unsigned>(Stat::Count)];

inline void stat_add(Stat stat, uint64_t n = 1)
{
    if (g_stats_enabled)
        g_stat_values[static_cast<unsigned>(stat)].fetch_add(n, std::memory_order_relaxed);
}

// Measures a scope into Stat::PsoNanos (only when statistics are on).
struct PsoTimer {
    uint64_t start = g_stats_enabled ? now() : 0;
    ~PsoTimer()
    {
        if (g_stats_enabled) {
            stat_add(Stat::PsoNanos, now() - start);
            stat_add(Stat::PsoTotal);
        }
    }
    static uint64_t now();
};

// Called by every Present; prints a report when enabled and enough frames have passed.
void stats_frame();

} // namespace d3d12m
