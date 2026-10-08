// Unix side of the Wine unix-call transport: Wine's loader finds
// __wine_unix_call_funcs in d3d12metal.so; each entry unpacks the parameters the
// PE client packed and calls the Metal backend.
#import <Foundation/Foundation.h>

#include "bridge/mtlb.h"
#include "bridge/wine/mtlb_wine_common.h"
#include "mtlb_wine_params.h"

#define MTLB_WINE_SIDE "unix"
#define MTLB_UNIX_POOL_BEGIN @autoreleasepool {
#define MTLB_UNIX_POOL_END }
#define MTLB_UNIX_TABLE_EXPORT __attribute__((visibility("default")))

typedef int32_t (*unixlib_entry_t)(void *);

extern "C" {
#include "mtlb_wine_unix.inc"
}
