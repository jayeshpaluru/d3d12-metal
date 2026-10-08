// Minimal helpers for the headless tests: each test is a plain executable that
// returns non-zero on the first failed check.
#pragma once

#include <cstdio>
#include <cstdlib>

#include "common/d3d12_uuids.h"

#include <wsl/wrladapter.h>

using Microsoft::WRL::ComPtr;

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
