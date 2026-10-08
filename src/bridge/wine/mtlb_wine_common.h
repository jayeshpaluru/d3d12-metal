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

/* Per-call tracing on stderr when D3D12METAL_LOG is set to something but 0. */
static inline int mtlb_wine_log_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("D3D12METAL_LOG");
        enabled = value && *value && *value != '0';
    }
    return enabled;
}
#define MTLB_WINE_LOG() mtlb_wine_log_enabled()

static inline void mtlb_wine_trace_call(const char *side, const char *name, const uint64_t *args, unsigned count)
{
    char line[256];
    int length = snprintf(line, sizeof(line), "d3d12-metal[%s]: %s(", side, name);
    for (unsigned i = 0; i < count && length > 0 && length < (int)sizeof(line) - 24; ++i)
        length += snprintf(line + length, sizeof(line) - length, i ? ", 0x%llx" : "0x%llx", (unsigned long long)args[i]);
    fprintf(stderr, "%s)\n", line);
}

static inline void mtlb_wine_trace_result(const char *side, const char *name, int64_t value, int has_value)
{
    if (has_value)
        fprintf(stderr, "d3d12-metal[%s]:   %s -> %lld\n", side, name, (long long)value);
}

#endif
