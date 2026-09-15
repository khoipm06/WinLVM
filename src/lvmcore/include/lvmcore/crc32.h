/*
 * crc32.h — LVM2's CRC-32. Poly 0xedb88320, init 0xf597a6cf, reflected,
 * NO final XOR. NOT the same as zlib crc32 (different init and no final
 * inversion). Must match winlvm_lvm_crc32 byte-for-byte (differential gate).
 */
#ifndef LVMCORE_CRC32_H
#define LVMCORE_CRC32_H

#include <stddef.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_CRC32_H */
