/*
 * luks_read.h — freestanding LUKS2 header and JSON parser.
 *
 * Implements Phase 5 of winlvm. Like lvm_read.h, this is designed for
 * dual-mode use: built for user-mode tests (with normal C stdlib) and
 * for kernel/UEFI (freestanding, no exceptions, no CRT).
 *
 * Field layout verified against third_party/cryptsetup/lib/luks2/luks2.h
 * and luks2_segment.c / luks2_keyslot.c / luks2_digest.c upstream.
 */
#ifndef WINLVM_DRIVER_LUKS_READ_H
#define WINLVM_DRIVER_LUKS_READ_H

#include <stddef.h>
#include <stdint.h>

#include "../winlvm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sizes taken verbatim from third_party/cryptsetup/lib/luks2/luks2.h */
#define LUKS2_MAGIC_L             6
#define LUKS2_UUID_L             40
#define LUKS2_LABEL_L            48
#define LUKS2_SALT_L             64
#define LUKS2_CHECKSUM_ALG_L     32
#define LUKS2_CHECKSUM_L         64
#define LUKS2_HDR_BIN_LEN        4096

#define LUKS2_MAGIC_1ST           "LUKS\xba\xbe"
#define LUKS2_MAGIC_2ND           "SKUL\xba\xbe"

#define LUKS2_DEFAULT_HDR_SIZE    0x1000000ull   /* 16 MiB */
#define LUKS2_HDR_16K_LEN         0x4000ull      /* 16 KiB (minimum) */

/* Basic error reporting. */
typedef struct {
    int  failed;
    char message[160];
} winlvm_luks_err;

/* Primary binary header (4096 bytes, packed). */
typedef struct {
    char     magic[LUKS2_MAGIC_L];
    uint16_t version;
    uint64_t hdr_size;            /* includes JSON area */
    uint64_t seqid;
    char     label[LUKS2_LABEL_L];
    char     checksum_alg[LUKS2_CHECKSUM_ALG_L];
    uint8_t  salt[LUKS2_SALT_L];
    char     uuid[LUKS2_UUID_L];
    char     subsystem[LUKS2_LABEL_L];
    uint64_t hdr_offset;          /* absolute byte offset of this header */
    uint8_t  csum[LUKS2_CHECKSUM_L];
} winlvm_luks_header;

/* JSON object — one segment, keyslot, or digest. */
typedef struct {
    char     type[64];            /* e.g. "crypt", "luks2", "pbkdf2" */
    uint64_t offset;              /* bytes */
    uint64_t size;                /* bytes */
    uint64_t iv_tweak;            /* sector number origin */
    uint64_t sector_size;         /* bytes (typically 512) */
    char     encryption[64];      /* cipher spec, e.g. "aes-xts-plain64" */
    char     hash[64];            /* KDF hash, e.g. "sha256" */
    uint64_t iterations;          /* KDF iterations (PBKDF2) */
    uint64_t time;                /* Argon2 time cost */
    uint64_t memory;              /* Argon2 memory (KB) */
    uint64_t cpus;                /* Argon2 threads */
} winlvm_luks_kdf;

/* LUKS2 segment. From luks2_segment.c: type/offset/size/iv_tweak/encryption/sector_size */
typedef struct {
    char     type[64];
    uint64_t offset;
    uint64_t size;
    uint64_t iv_tweak;
    uint64_t sector_size;
    char     encryption[64];
} winlvm_luks_segment;

#define WL_LUKS_B64_MAX 128   /* base64 chars we store per field */

/* LUKS2 digest entry. From luks2_digest.c: type/segments/keyslots.
 * Subtype (pbkdf2, etc.) carries hash/iterations/salt/digest. */
typedef struct {
    char     type[64];
    winlvm_luks_kdf kdf;
    char     salt_b64[WL_LUKS_B64_MAX];
    char     digest_b64[WL_LUKS_B64_MAX];
} winlvm_luks_digest;

/* LUKS2 keyslot. From luks2_keyslot.c: type/priority/kdf/area(offset,size) */
typedef struct {
    char     type[64];
    uint64_t priority;
    winlvm_luks_kdf kdf;
    uint64_t area_offset;
    uint64_t area_size;
    uint64_t key_size;            /* volume-key bytes (64 for XTS-AES-256) */
    char     kdf_salt_b64[WL_LUKS_B64_MAX];
    /* Anti-forensic splitter, af: { type, stripes, hash } */
    char     af_type[64];         /* "luks1" */
    uint64_t af_stripes;          /* 4000 by default */
    char     af_hash[64];         /* "sha256" */
} winlvm_luks_keyslot;

/* Top-level parsed metadata. */
typedef struct {
    int      segment_count;
    winlvm_luks_segment segments[8];
    int      keyslot_count;
    winlvm_luks_keyslot keyslots[8];
    int      digest_count;
    winlvm_luks_digest  digests[8];
} winlvm_luks_metadata;

/* Read & validate the primary 4096-byte binary header.
 * Returns 1 on success, 0 on error.
 * Bytes are read using the caller-supplied src. */

int winlvm_luks_read_header(winlvm_byte_source *src,
                            uint64_t hdr_offset,
                            winlvm_luks_header *out,
                            winlvm_luks_err *err);

/* Read & parse the full LUKS2 metadata: primary header + JSON area.
 * Fills out_md. */
int winlvm_luks_read_metadata(winlvm_byte_source *src,
                              winlvm_luks_metadata *out_md,
                              winlvm_luks_err *err);

#ifdef __cplusplus
}
#endif
#endif

