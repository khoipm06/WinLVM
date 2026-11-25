/*
 * block_io.h — Abstract freestanding read/write byte-source interface for
 *              lvmcore on-disk block engines (dm-thin, dm-snapshot, dm-raid1/mirror).
 */
#ifndef LVMCORE_BLOCK_IO_H
#define LVMCORE_BLOCK_IO_H

#include "lvmcore/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Random-access read/write byte source representing a backing sub-LV
 * (e.g., <pool>_tmeta, <pool>_tdata, <orig>, <snap>_cow, <lv>_rimage_N).
 * Callbacks return 0 on full transfer success, non-zero on I/O failure.
 */
typedef struct lvmcore_rw_source {
    void    *impl;
    uint64_t size;
    int (*read)(void *impl, uint64_t offset, void *buf, size_t len);
    int (*write)(void *impl, uint64_t offset, const void *buf, size_t len);
} lvmcore_rw_source;

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_BLOCK_IO_H */
