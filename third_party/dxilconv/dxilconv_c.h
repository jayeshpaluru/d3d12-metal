/* C interface to the DXBC -> DXIL converter (third_party/dxilconv, built by tools/build-dxilconv.sh into
 * libdxilconv.dylib). */
#ifndef DXILCONV_C_H
#define DXILCONV_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DXILCONV_EXPORT __attribute__((visibility("default")))

/* Converts the DXBC container `dxbc` into a DXIL container. Returns 0 (S_OK) and a malloc'd blob in *dxil, or an
 * HRESULT (negative) and, when `diag` is given, the converter's messages. Free the blob with dxilconv_free.
 * Thread safe. */
DXILCONV_EXPORT int dxilconv_convert(const void *dxbc, uint32_t dxbc_size, void **dxil, uint32_t *dxil_size,
                                     char *diag, uint32_t diag_size);
DXILCONV_EXPORT void dxilconv_free(void *dxil);

/* Rewrites typed-buffer accesses of a DXIL container for windowed typed buffers: Metal texture buffers hold at most
 * 2^28 elements, so for every typed buffer (Buffer<T>/RWBuffer<T>) resource R in space s, registers [lb, ub], a window
 * resource Rw of the same class/kind/element type is declared in space s + window_space_offset, registers
 * [3 * lb, 3 * ub + 2] (unbounded stays unbounded). Window w (1..3) of register r of R is register 3 * r + (w - 1) of
 * Rw and holds the elements from w * 2^28 on. Loads, stores and atomics with an index >= 2^28 are redirected to the
 * window min(index >> 28, 3) at index - (w << 28).
 * Returns 1 and a malloc'd container in *out (free with dxilconv_free) when the shader was rewritten, 0 when it has no
 * typed buffer access to rewrite (*out untouched), or a negative value on failure with a message in diag.
 * Thread safe. */
DXILCONV_EXPORT int dxilconv_window_typed_buffers(const void *dxil, uint32_t size, uint32_t window_space_offset,
                                                  void **out, uint32_t *out_size, char *diag, uint32_t diag_size);

#ifdef __cplusplus
}
#endif

#endif
