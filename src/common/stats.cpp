#include "common/stats.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "bridge/mtlb.h"
#include "common/config.h"
#include "common/log.h"

#ifdef _WIN32
// Bridge calls (unix calls) made so far; counted by the PE client (src/bridge/wine/mtlb_client.cpp).
extern "C" uint64_t mtlb_client_call_count(void);
#else
static uint64_t mtlb_client_call_count(void) { return 0; }  // native builds call the backend directly
#endif

namespace d3d12m {

namespace {
constexpr unsigned kFramesPerReport = 120;
} // namespace

bool g_stats_enabled = config_flag("STATS");

uint64_t PsoTimer::now()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::atomic<uint64_t> g_stat_values[static_cast<unsigned>(Stat::Count)];

void stats_frame()
{
    if (!g_stats_enabled)
        return;
    static std::atomic<unsigned> frames{0};
    if (frames.fetch_add(1) + 1 < kFramesPerReport)
        return;
    frames = 0;

    static uint64_t last_front[static_cast<unsigned>(Stat::Count)];
    static mtlb_stats last_back;
    static uint64_t last_calls;
    uint64_t front[static_cast<unsigned>(Stat::Count)];
    for (unsigned i = 0; i < static_cast<unsigned>(Stat::Count); ++i)
        front[i] = g_stat_values[i].load();
    mtlb_stats back;
    mtlb_stats_get(&back);
    const uint64_t calls = mtlb_client_call_count();
    auto per_frame = [](uint64_t now, uint64_t before) { return double(now - before) / kFramesPerReport; };
    auto f = [&](Stat s) { return per_frame(front[static_cast<unsigned>(s)], last_front[static_cast<unsigned>(s)]); };
    static uint64_t last_time;
    const uint64_t time = PsoTimer::now();
    const double seconds = last_time ? (time - last_time) / 1e9 : 0;
    last_time = time;
    log_printf(
                 "d3d12-metal stats (per frame, last %u): fps %.1f, gpu %.2f ms (busy %.2f), submits %.1f, lists %.1f, command buffers %.1f, render passes %.1f, "
                 "compute encoders %.1f, blit encoders %.1f, barriers %.1f, fence syncs %.1f, descriptor writes %.0f, "
                 "PSO creations %.2f, stream KB %.1f, unix calls %.1f; PSO creation total %llu in %.1f ms; "
                 "ms per frame in submit %.2f, present %.2f, signal %.2f",
                 kFramesPerReport, seconds > 0 ? kFramesPerReport / seconds : 0.0,
                 per_frame(back.gpu_nanos, last_back.gpu_nanos) / 1e6,
                 per_frame(back.gpu_busy_nanos, last_back.gpu_busy_nanos) / 1e6, f(Stat::Submits), f(Stat::CommandLists), per_frame(back.command_buffers, last_back.command_buffers),
                 per_frame(back.render_encoders, last_back.render_encoders), per_frame(back.compute_encoders, last_back.compute_encoders),
                 per_frame(back.blit_encoders, last_back.blit_encoders), per_frame(back.barriers, last_back.barriers),
                 per_frame(back.syncs, last_back.syncs), f(Stat::DescriptorWrites), f(Stat::PsoCreations),
                 f(Stat::StreamBytes) / 1024.0, per_frame(calls, last_calls),
                 static_cast<unsigned long long>(front[static_cast<unsigned>(Stat::PsoTotal)]),
                 front[static_cast<unsigned>(Stat::PsoNanos)] / 1e6, f(Stat::SubmitNanos) / 1e6, f(Stat::PresentNanos) / 1e6,
                 f(Stat::SignalNanos) / 1e6);
    for (unsigned i = 0; i < static_cast<unsigned>(Stat::Count); ++i)
        last_front[i] = front[i];
    last_back = back;
    last_calls = calls;
    profile_report(kFramesPerReport);
}

} // namespace d3d12m
