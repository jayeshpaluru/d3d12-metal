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

#ifdef __cplusplus
}
#endif

#endif
