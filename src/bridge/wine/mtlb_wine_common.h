// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Pieces of the Wine unix-call transport shared by the PE client and the unix
 * module. The per-function parts (indices, parameter structs, thunks) are
 * generated from mtlb.h by tools/gen_mtlb_wine.py.
 */
#ifndef MTLB_WINE_COMMON_H
#define MTLB_WINE_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
#ifdef _WIN32
int d3d12m_option_enabled(const char *key);                   /* src/common/config.cpp */
void d3d12m_log_text(const char *text, size_t length);        /* src/common/log.cpp */
#else
extern volatile int d3d12metal_option_epoch;                  /* bumped by mtlb_configure */
void d3d12metal_log_text(const char *text, size_t length);    /* src/bridge/metal/log.cpp */
#endif
#ifdef __cplusplus
}
#endif

/* Per-call tracing when D3D12METAL_LOG is set to something but 0 (environment or d3d12metal.conf). */
#ifdef _WIN32
static inline int mtlb_wine_log_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = d3d12m_option_enabled("LOG");
    return enabled;
}
static inline void mtlb_wine_emit(const char *text, size_t length) { d3d12m_log_text(text, length); }
#else
static inline int mtlb_wine_log_enabled(void)
{
    /* The PE side tells this side the options of its configuration file first thing (mtlb_configure). */
    static int enabled = 0, seen_epoch = -1;
    const int epoch = d3d12metal_option_epoch;
    if (seen_epoch != epoch) {
        const char *value = getenv("D3D12METAL_LOG");
        enabled = value && *value && *value != '0';
        seen_epoch = epoch;
    }
    return enabled;
}
static inline void mtlb_wine_emit(const char *text, size_t length) { d3d12metal_log_text(text, length); }
#endif
#define MTLB_WINE_LOG() mtlb_wine_log_enabled()

static inline void mtlb_wine_trace_call(const char *side, const char *name, const uint64_t *args, unsigned count)
{
    char line[256];
    int length = snprintf(line, sizeof(line), "d3d12-metal[%s]: %s(", side, name);
    for (unsigned i = 0; i < count && length > 0 && length < (int)sizeof(line) - 24; ++i)
        length += snprintf(line + length, sizeof(line) - length, i ? ", 0x%llx" : "0x%llx", (unsigned long long)args[i]);
    length += snprintf(line + length, sizeof(line) - length, ")\n");
    mtlb_wine_emit(line, length < (int)sizeof(line) ? (size_t)length : sizeof(line) - 1);
}

static inline void mtlb_wine_trace_result(const char *side, const char *name, int64_t value, int has_value)
{
    if (has_value) {
        char line[128];
        int length = snprintf(line, sizeof(line), "d3d12-metal[%s]:   %s -> %lld\n", side, name, (long long)value);
        mtlb_wine_emit(line, length < (int)sizeof(line) ? (size_t)length : sizeof(line) - 1);
    }
}

#endif
