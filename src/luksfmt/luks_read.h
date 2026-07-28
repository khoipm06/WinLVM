/*
 * luks_read.h — freestanding LUKS2 header and JSON parser.
 *
 * Implements Phase 5 of winlvm. Like lvm_read.h, this is designed for
 * dual-mode use: built for user-mode tests (with normal C stdlib) and
 * for kernel/UEFI (freestanding, no exceptions, no CRT).
 */
#ifndef WINLVM_DRIVER_LUKS_READ_H
#define WINLVM_DRIVER_LUKS_READ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Basic error reporting. */
typedef struct {
    int  failed;
    char message[160];
} winlvm_luks_err;

/*
 * 1. Primary Binary Header
 */
typedef struct {
    char     magic[6];           /* "LUKS\xba\xbe" */
    uint16_t version;            /* 2 */
    uint64_t hdr_size;           /* bytes, typically 16384 */
    uint64_t seqid;
    char     label[48];
    char     checksum_alg[32];
    uint8_t  salt[64];
    char     uuid[40];
    char     subsystem[48];
    uint64_t hdr_offset;         /* absolute byte offset of this header */
    uint8_t  csum[64];
} winlvm_luks_header;

/* Forward declaration for the JSON parser result. */
typedef struct {
    /* TODO: Parse the JSON into structured keyslots, segments, digests. */
    int has_json;
} winlvm_luks_json;

/* The entry point to read and validate the primary header.
 * Input buffer must be at least 4096 bytes. */
int winlvm_luks_read_header(const void *buf, size_t bufsz,
                            winlvm_luks_header *out,
                            winlvm_luks_err *err);

#ifdef __cplusplus
}
#endif
#endif
