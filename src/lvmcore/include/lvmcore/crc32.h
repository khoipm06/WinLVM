/*
 * crc32.h — LVM2's CRC-32. Poly 0xedb88320, init 0xf597a6cf, reflected,
 * NO final XOR. NOT the same as zlib crc32 (different init and no final
 * inversion). Must match winlvm_lvm_crc32 byte-for-byte (differential gate).
 */
#ifndef LVMCORE_CRC32_H
#define LVMCORE_CRC32_H

#include "lvmcore/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Seed value; crc of zero bytes returns this. Matches third_party/lvm2
 * lib/misc/crc.h INITIAL_CRC. */
#define LVMCORE_CRC32_INIT 0xf597a6cfu

/* Compute LVM2 CRC-32 over data[0..len). Reentrant, no statics. */
uint32_t lvmcore_crc32(const void *data, size_t len);

/* Incremental: seed with LVMCORE_CRC32_INIT or previous return. */
uint32_t lvmcore_crc32_update(uint32_t crc, const void *data, size_t len);

/*
 * Castagnoli CRC-32C (polynomial 0x82f63b78 reflected) matching Linux
 * device-mapper persistent-data (`dm_bm_checksum`: crc32c(~0u, buf, len) ^ xor_val,
 * with NO final bitwise inversion before the XOR constant).
 */
#define LVMCORE_CRC32C_INIT                0xffffffffu
#define LVMCORE_THIN_SUPERBLOCK_CSUM_XOR   160774u
#define LVMCORE_THIN_BTREE_CSUM_XOR        121107u
#define LVMCORE_THIN_BITMAP_CSUM_XOR       240779u
#define LVMCORE_THIN_INDEX_CSUM_XOR        160478u

uint32_t lvmcore_crc32c_update(uint32_t crc, const void *data, size_t len);
uint32_t lvmcore_crc32c_dm(const void *data, size_t len, uint32_t xor_val);

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_CRC32_H */
