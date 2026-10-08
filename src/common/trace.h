// API trace (D3D12METAL_TRACE=1, or trace=1 in d3d12metal.conf): every D3D12 and DXGI method an application
// calls is logged with its arguments and result, and the set of distinct methods used (with call counts) is
// kept in <log file>.methods. This is how a game's needs are inventoried.
//
// A method opts in with one line at the top of its body:
//     D3D12M_TRACE(arg1, arg2, ...);            // void and non-HRESULT methods: logged on entry
// or, for a method returning HRESULT, around the whole body (the result is logged):
//     D3D12M_TRACED_BEGIN
//     ...body...
//     D3D12M_TRACED_END(arg1, arg2, ...)
// The arguments are the names of the parameters worth showing. Off, the cost is one predictable branch.
//
// To keep a long run readable, a method is logged for its first kTraceFirst calls and then every
// kTraceEvery-th call; the counts in the method list are exact.
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "common/d3d12_uuids.h"
#include "dxgi/dxgi_interfaces.h"

// The printf flavour of the C library (MinGW's own, which has %z and %ll, under Windows).
#ifdef __MINGW32__
#define D3D12M_PRINTF_FORMAT __MINGW_PRINTF_FORMAT
#else
#define D3D12M_PRINTF_FORMAT printf
#endif

namespace d3d12m {

extern bool g_trace_enabled;

constexpr uint64_t kTraceFirst = 50;
constexpr uint64_t kTraceEvery = 1000;

struct TraceSite {
    const char *function;  // __PRETTY_FUNCTION__ of the traced method
    std::atomic<uint64_t> count{0};
};

// A line being built.
class TraceLine {
public:
    void printf(const char *format, ...) __attribute__((format(D3D12M_PRINTF_FORMAT, 2, 3)));
    void append(const char *text, size_t length);
    const char *data() const { return text_; }
    size_t size() const { return size_; }

private:
    char text_[1536];
    size_t size_ = 0;
};

// Counts a call and decides whether it is logged (the first kTraceFirst, then every kTraceEvery-th).
bool trace_count(TraceSite &site, uint64_t *count);
// Writes "<method>(<args>)[ = <result>]" to the log. `args` was formatted by the caller.
void trace_emit(const TraceSite &site, uint64_t count, const TraceLine &args, const HRESULT *result);
// A stub (D3D12M_STUB_LOG): counted and logged like a traced method, without arguments.
void trace_stub(const char *function);
// Rewrites the method list when it is time (called from Present).
void trace_frame();

const char *trace_iid_name(const GUID &iid, char *buffer, size_t size);

// ---------------------------------------------------------------------------
// Argument formatting
// ---------------------------------------------------------------------------

// A one-line summary of what a pointer to a descriptor structure points to (resource description, swap chain
// description, ...), appended after the pointer. The generic version says nothing.
template <typename T>
void trace_describe(TraceLine &, const T *) {}
void trace_describe(TraceLine &, const D3D12_RESOURCE_DESC *);
void trace_describe(TraceLine &, const D3D12_RESOURCE_DESC1 *);
void trace_describe(TraceLine &, const D3D12_HEAP_PROPERTIES *);
void trace_describe(TraceLine &, const D3D12_HEAP_DESC *);
void trace_describe(TraceLine &, const D3D12_COMMAND_QUEUE_DESC *);
void trace_describe(TraceLine &, const D3D12_DESCRIPTOR_HEAP_DESC *);
void trace_describe(TraceLine &, const D3D12_QUERY_HEAP_DESC *);
void trace_describe(TraceLine &, const D3D12_COMMAND_SIGNATURE_DESC *);
void trace_describe(TraceLine &, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *);
void trace_describe(TraceLine &, const D3D12_COMPUTE_PIPELINE_STATE_DESC *);
void trace_describe(TraceLine &, const D3D12_PIPELINE_STATE_STREAM_DESC *);
void trace_describe(TraceLine &, const D3D12_VIEWPORT *);
void trace_describe(TraceLine &, const D3D12_CLEAR_VALUE *);
void trace_describe(TraceLine &, const D3D12_SHADER_RESOURCE_VIEW_DESC *);
void trace_describe(TraceLine &, const D3D12_UNORDERED_ACCESS_VIEW_DESC *);
void trace_describe(TraceLine &, const D3D12_RENDER_TARGET_VIEW_DESC *);
void trace_describe(TraceLine &, const D3D12_DEPTH_STENCIL_VIEW_DESC *);
void trace_describe(TraceLine &, const DXGI_SWAP_CHAIN_DESC *);
void trace_describe(TraceLine &, const DXGI_SWAP_CHAIN_DESC1 *);
void trace_describe(TraceLine &, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *);
void trace_describe(TraceLine &, const D3D12_RESOURCE_BARRIER *);

void trace_text(TraceLine &line, const wchar_t *text);
void trace_text(TraceLine &line, const char *text);
void trace_guid(TraceLine &line, const GUID &iid);

template <typename T>
void trace_value(TraceLine &line, const T &value)
{
    using D = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<D, const wchar_t *> || std::is_same_v<D, wchar_t *>) {
        trace_text(line, value);
    } else if constexpr (std::is_same_v<D, const char *> || std::is_same_v<D, char *>) {
        trace_text(line, value);
    } else if constexpr (std::is_pointer_v<D>) {
        line.printf("%p", static_cast<const void *>(value));
        if constexpr (!std::is_void_v<std::remove_pointer_t<D>> && !std::is_function_v<std::remove_pointer_t<D>>) {
            if (value)
                trace_describe(line, value);
        }
    } else if constexpr (std::is_same_v<D, bool>) {
        line.printf("%s", value ? "true" : "false");
    } else if constexpr (std::is_enum_v<D>) {
        line.printf("%lld", static_cast<long long>(value));
    } else if constexpr (std::is_integral_v<D>) {
        if constexpr (std::is_signed_v<D>)
            line.printf("%lld", static_cast<long long>(value));
        else
            line.printf("%llu", static_cast<unsigned long long>(value));
    } else if constexpr (std::is_floating_point_v<D>) {
        line.printf("%g", static_cast<double>(value));
    } else if constexpr (std::is_same_v<D, GUID>) {
        trace_guid(line, value);
    } else if constexpr (std::is_same_v<D, D3D12_CPU_DESCRIPTOR_HANDLE> || std::is_same_v<D, D3D12_GPU_DESCRIPTOR_HANDLE>) {
        line.printf("0x%llx", static_cast<unsigned long long>(value.ptr));
    } else if constexpr (std::is_same_v<D, LUID>) {
        line.printf("%08x:%08x", static_cast<unsigned>(value.HighPart), static_cast<unsigned>(value.LowPart));
    } else {
        line.printf("{...}");
    }
}

// Formats `names` (the macro's stringified, comma-separated argument list) and the values as "a=1, b=0x2".
template <typename... Args>
void trace_args(TraceLine &line, const char *names, const Args &...args)
{
    const char *cursor = names;
    bool first = true;
    auto one = [&](const auto &value) {
        while (*cursor == ' ' || *cursor == ',')
            ++cursor;
        const char *end = cursor;
        while (*end && *end != ',')
            ++end;
        if (!first)
            line.append(", ", 2);
        first = false;
        line.append(cursor, static_cast<size_t>(end - cursor));
        line.append("=", 1);
        trace_value(line, value);
        cursor = end;
    };
    (one(args), ...);
    (void)one;
}

template <typename... Args>
void trace_call(TraceSite &site, const char *names, const Args &...args)
{
    uint64_t count;
    if (!trace_count(site, &count))
        return;
    TraceLine line;
    trace_args(line, names, args...);
    trace_emit(site, count, line, nullptr);
}

template <typename... Args>
void trace_call_result(TraceSite &site, HRESULT result, const char *names, const Args &...args)
{
    uint64_t count;
    if (!trace_count(site, &count))
        return;
    TraceLine line;
    trace_args(line, names, args...);
    trace_emit(site, count, line, &result);
}

} // namespace d3d12m

#define D3D12M_TRACE_UNLIKELY(x) __builtin_expect(!!(x), 0)

// First line of a method that returns nothing or a plain value.
#define D3D12M_TRACE(...)                                                               \
    do {                                                                                \
        if (D3D12M_TRACE_UNLIKELY(::d3d12m::g_trace_enabled)) {                         \
            static ::d3d12m::TraceSite d3d12m_site{__PRETTY_FUNCTION__};                \
            ::d3d12m::trace_call(d3d12m_site, #__VA_ARGS__ __VA_OPT__(,) __VA_ARGS__);             \
        }                                                                               \
    } while (0)

// Around the body of a method that returns HRESULT: every path of the body must return (it is a lambda).
#define D3D12M_TRACED_BEGIN const auto d3d12m_body = [&]() -> HRESULT {
#define D3D12M_TRACED_END(...)                                                          \
    };                                                                                  \
    if (!D3D12M_TRACE_UNLIKELY(::d3d12m::g_trace_enabled))                              \
        return d3d12m_body();                                                           \
    const HRESULT d3d12m_result = d3d12m_body();                                        \
    {                                                                                   \
        static ::d3d12m::TraceSite d3d12m_site{__PRETTY_FUNCTION__};                    \
        ::d3d12m::trace_call_result(d3d12m_site, d3d12m_result, #__VA_ARGS__ __VA_OPT__(,) __VA_ARGS__); \
    }                                                                                   \
    return d3d12m_result;
