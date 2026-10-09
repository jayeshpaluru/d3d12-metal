// SPDX-License-Identifier: LGPL-2.1-or-later
// Logging helpers shared by the D3D12 and DXGI front-ends.
//
// Messages go to stderr and, when D3D12METAL_LOG_FILE is set (environment or d3d12metal.conf, see
// common/config.h), are appended to that file by both sides of a Wine process.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdio>

#include "common/trace.h"

namespace d3d12m {

// Appends whole lines (each with its newline) to stderr and the log file. Thread-safe.
void log_text(const char *text, size_t length);
// Formats one line (a newline is added).
void log_printf(const char *format, ...) __attribute__((format(D3D12M_PRINTF_FORMAT, 1, 2)));

} // namespace d3d12m

// Logs a printf-style message.
#define D3D12M_LOG(...) ::d3d12m::log_printf("d3d12-metal: " __VA_ARGS__)

// Logs "<function> is not implemented" the first time this call site runs. With the API trace on, every call
// is counted.
#define D3D12M_STUB_LOG()                                                         \
    do {                                                                          \
        static std::atomic<bool> logged{false};                                   \
        if (!logged.exchange(true))                                               \
            D3D12M_LOG("%s is not implemented", __PRETTY_FUNCTION__);             \
        if (D3D12M_TRACE_UNLIKELY(::d3d12m::g_trace_enabled))                     \
            ::d3d12m::trace_stub(__PRETTY_FUNCTION__);                            \
    } while (0)

// Body of an unsupported method that returns an HRESULT.
#define D3D12M_STUB_HR()   \
    do {                   \
        D3D12M_STUB_LOG(); \
        return E_NOTIMPL;  \
    } while (0)
