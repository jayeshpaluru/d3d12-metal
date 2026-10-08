// Minimal helpers for the headless tests: each test is a plain executable that
// returns non-zero on the first failed check.
#pragma once

#include <cstdio>
#include <cstdlib>

#include "common/d3d12_uuids.h"

// d3d12.dll entry points (not declared by DirectX-Headers).
extern "C" {
HRESULT D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void **ppDevice);
HRESULT D3D12GetDebugInterface(REFIID riid, void **ppvDebug);
}

// Aborts the test with a message when `cond` is false.
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,  \
                         #cond);                                                   \
            std::exit(1);                                                          \
        }                                                                          \
    } while (0)

// Aborts the test unless the expression returns a successful HRESULT.
#define CHECK_HR(expr)                                                             \
    do {                                                                           \
        HRESULT hr_ = (expr);                                                      \
        if (FAILED(hr_)) {                                                         \
            std::fprintf(stderr, "%s:%d: %s failed: 0x%08x\n", __FILE__, __LINE__, \
                         #expr, static_cast<unsigned>(hr_));                       \
            std::exit(1);                                                          \
        }                                                                          \
    } while (0)

// Minimal RAII wrapper so tests do not leak COM objects.
template <typename T>
class Com {
public:
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    Com(Com &&other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    Com &operator=(Com &&other) noexcept
    {
        if (this != &other) {
            reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }
    ~Com() { reset(); }

    T *operator->() const { return p_; }
    T *get() const { return p_; }
    // Receives a new reference, releasing any held one.
    T **put()
    {
        reset();
        return &p_;
    }
    void reset()
    {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T *p_ = nullptr;
};
