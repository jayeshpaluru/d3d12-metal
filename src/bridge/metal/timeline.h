// SPDX-License-Identifier: LGPL-2.1-or-later
// Frame timeline (D3D12METAL_TIMELINE=<file.json>): CPU calls of the backend per thread and the GPU interval of every
// command buffer per queue, written as Chrome trace JSON (chrome://tracing, ui.perfetto.dev) after a window of presents.
// D3D12METAL_TIMELINE_START (presents before recording, default 600) and D3D12METAL_TIMELINE_FRAMES (presents recorded,
// default 60). Creating <file>.go starts a recording at the next multiple of 30 presents (the file is removed).
// With the variable unset every hook is one predictable branch.
#pragma once

#include <cstdint>

namespace mtlb::timeline {

bool enabled();
// Seconds on the clock of MTLCommandBuffer.GPUStartTime (mach_absolute_time).
double now();
// A call on the current thread from `begin` to now; `arg` is shown with it (a fence value, a queue id).
void cpu(const char *name, double begin, uint64_t arg = 0);
// A point in time on the current thread.
void instant(const char *name, uint64_t arg = 0);
// A command buffer's GPU interval on queue `queue`; `committed` is the CPU time of its commit.
void gpu(uint32_t queue, double begin, double end, double committed, uint64_t seq);
// Counts presents: opens and closes the recording window, writes the file when it closes.
void present();

// Records the enclosing scope as a call when the timeline is on.
class Scope {
public:
    explicit Scope(const char *name, uint64_t arg = 0) : name_(name), arg_(arg), begin_(enabled() ? now() : 0) {}
    ~Scope()
    {
        if (begin_)
            cpu(name_, begin_, arg_);
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    const char *name_;
    uint64_t arg_;
    double begin_;
};

} // namespace mtlb::timeline
