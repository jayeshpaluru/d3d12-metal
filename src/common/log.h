// Logging helpers shared by the D3D12 and DXGI front-ends.
#pragma once

#include <atomic>
#include <cstdio>

// Logs a printf-style message to stderr.
#define D3D12M_LOG(...)                                    \
    do {                                                   \
        std::fprintf(stderr, "d3d12-metal: " __VA_ARGS__); \
        std::fputc('\n', stderr);                          \
    } while (0)

// Logs "<function> is not implemented" the first time this call site runs.
#define D3D12M_STUB_LOG()                                             \
    do {                                                              \
        static std::atomic<bool> logged{false};                       \
        if (!logged.exchange(true))                                   \
            D3D12M_LOG("%s is not implemented", __PRETTY_FUNCTION__); \
    } while (0)

// Body of an unsupported method that returns an HRESULT.
#define D3D12M_STUB_HR()   \
    do {                   \
        D3D12M_STUB_LOG(); \
        return E_NOTIMPL;  \
    } while (0)
