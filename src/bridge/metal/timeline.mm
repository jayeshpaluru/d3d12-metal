// SPDX-License-Identifier: LGPL-2.1-or-later
#include "timeline.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "bridge/mtlb.h"
#include "log.h"

namespace mtlb::timeline {
namespace {

struct Event {
    const char *name;
    char phase;     // 'X' complete, 'i' instant
    uint32_t pid;   // 1 CPU, 2 GPU
    uint64_t tid;   // thread id, or queue id on the GPU
    double ts, dur; // seconds
    uint64_t arg;
    double extra;   // GPU: commit-to-start latency (seconds)
};

struct State {
    const char *path = getenv("D3D12METAL_TIMELINE");
    uint64_t start = getenv("D3D12METAL_TIMELINE_START") ? strtoull(getenv("D3D12METAL_TIMELINE_START"), nullptr, 10) : 600;
    uint64_t frames = getenv("D3D12METAL_TIMELINE_FRAMES") ? strtoull(getenv("D3D12METAL_TIMELINE_FRAMES"), nullptr, 10) : 60;
    std::atomic<uint64_t> presents{0};
    std::atomic<bool> recording{false};
    std::mutex mutex;
    std::vector<Event> events;
    std::map<uint64_t, std::string> thread_names;
};

State &state()
{
    static State *s = new State();  // never destroyed: completion handlers may run at exit
    return *s;
}

const bool g_enabled = getenv("D3D12METAL_TIMELINE") && *getenv("D3D12METAL_TIMELINE");

uint64_t thread_id()
{
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
}

void add(Event event, bool name_thread)
{
    State &s = state();
    if (!s.recording.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (name_thread && !s.thread_names.count(event.tid)) {
        char name[64] = {};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        s.thread_names[event.tid] = name;
    }
    s.events.push_back(event);
}

void json_string(FILE *f, const char *text)
{
    fputc('"', f);
    for (const char *p = text; *p; ++p) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        if (static_cast<unsigned char>(*p) >= 0x20)
            fputc(*p, f);
    }
    fputc('"', f);
}

void write_file(State &s)
{
    FILE *f = fopen(s.path, "w");
    if (!f) {
        backend_log("timeline: cannot write %s", s.path);
        return;
    }
    const double origin = s.events.empty() ? 0 : s.events.front().ts;
    fprintf(f, "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n");
    fprintf(f, "{\"ph\":\"M\",\"pid\":1,\"name\":\"process_name\",\"args\":{\"name\":\"CPU\"}},\n");
    fprintf(f, "{\"ph\":\"M\",\"pid\":2,\"name\":\"process_name\",\"args\":{\"name\":\"GPU\"}}");
    for (const auto &[tid, name] : s.thread_names) {
        fprintf(f, ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%llu,\"name\":\"thread_name\",\"args\":{\"name\":", (unsigned long long)tid);
        json_string(f, (name.empty() ? "thread " + std::to_string(tid) : name).c_str());
        fprintf(f, "}}");
    }
    for (const Event &e : s.events) {
        fprintf(f, ",\n{\"ph\":\"%c\",\"pid\":%u,\"tid\":%llu,\"ts\":%.3f,", e.phase, e.pid, (unsigned long long)e.tid,
                (e.ts - origin) * 1e6);
        if (e.phase == 'X')
            fprintf(f, "\"dur\":%.3f,", e.dur * 1e6);
        else
            fprintf(f, "\"s\":\"t\",");
        fprintf(f, "\"name\":");
        json_string(f, e.name);
        if (e.pid == 2)
            fprintf(f, ",\"args\":{\"seq\":%llu,\"commit_to_start_us\":%.1f}}", (unsigned long long)e.arg, e.extra * 1e6);
        else
            fprintf(f, ",\"args\":{\"v\":%llu}}", (unsigned long long)e.arg);
    }
    fprintf(f, "\n]}\n");
    fclose(f);
    backend_log("timeline: %zu events written to %s", s.events.size(), s.path);
}

} // namespace

bool enabled()
{
    return g_enabled;
}

double now()
{
    static const double scale = [] {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        return static_cast<double>(info.numer) / info.denom * 1e-9;
    }();
    return mach_absolute_time() * scale;
}

void cpu(const char *name, double begin, uint64_t arg)
{
    if (!g_enabled)
        return;
    const double end = now();
    add({name, 'X', 1, thread_id(), begin, end - begin, arg, 0}, true);
}

void instant(const char *name, uint64_t arg)
{
    if (!g_enabled)
        return;
    add({name, 'i', 1, thread_id(), now(), 0, arg, 0}, true);
}

void gpu(uint32_t queue, double begin, double end, double committed, uint64_t seq)
{
    if (!g_enabled)
        return;
    static const char *names[] = {"queue 0", "queue 1", "queue 2", "queue 3", "queue 4", "queue 5", "queue 6", "queue 7"};
    add({queue < 8 ? names[queue] : "queue", 'X', 2, queue, begin, end - begin, seq, begin - committed}, false);
}

void present()
{
    if (!g_enabled)
        return;
    State &s = state();
    const uint64_t n = ++s.presents;
    // <file>.go starts a recording at once (checked every 30 presents, then removed): captures of a scene reached later.
    static const std::string trigger = std::string(s.path) + ".go";
    if (!s.recording.load() && n % 30 == 0 && unlink(trigger.c_str()) == 0)
        s.start = n;
    if (n == s.start) {
        s.recording.store(true);
        backend_log("timeline: recording %llu presents", (unsigned long long)s.frames);
    } else if (n == s.start + s.frames) {
        s.recording.store(false);
        std::lock_guard<std::mutex> lock(s.mutex);
        write_file(s);
        s.events.clear();
    }
    instant("present", n);
}

} // namespace mtlb::timeline

extern "C" uint64_t mtlb_timeline_enabled(void)
{
    return mtlb::timeline::enabled() ? 1 : 0;
}

extern "C" void mtlb_timeline_mark(const char *name, uint64_t value)
{
    if (name)
        mtlb::timeline::instant(name, value);
}
