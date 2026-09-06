/*
 * luks_unlock.h — LUKS2 keyslot unlock + sector decrypt (Phase 5, Stage 4).
 *
 * Chain per cryptsetup luks2_keyslot.c / luks2_digest.c and the public
 * LUKS1 on-disk spec (anti-forensic splitter):
 *   area_key   = PBKDF2-HMAC-SHA256(passphrase, kdf_salt, iterations,
 *                                   area key_size)
 *   area bytes = XTS-decrypt(keyslot area) using area_key, plain64 sector
 *                numbering starting at 0 at area_offset
 *   volume_key = AF_merge(area bytes, stripes, af_hash)   [key_size bytes]
 *   verify     = PBKDF2-HMAC-SHA256(volume_key, digest_salt, digest_iter,
 *                                    32) == stored digest
 *   data       = XTS-decrypt(segment) using volume_key, plain64 sector
 *                numbering starting at 0 at segment data offset
 *
 * Fail closed: only pbkdf2 + luks1-AF(shas256) keyslots are unlockable;
 * anything else returns an error rather than guessing.
 */
#ifndef WINLVM_DRIVER_LUKS_UNLOCK_H
#define WINLVM_DRIVER_LUKS_UNLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "luks_read.h"

/* Maximum AF key material we process: key_size 64 * stripes 4000 = 256000. */
#define WL_LUKS_MAX_KEY_SIZE 64
#define WL_LUKS_MAX_STRIPES  8192

typedef struct {
    int  failed;
    char message[160];
} winlvm_luks_unlock_err;

/* Unlock one keyslot with the given passphrase. On success fills
 * volume_key (up to 64 bytes) and returns the key length.
 * scratch must hold key_size * af_stripes bytes — for cryptsetup defaults
 * that is 64 * 4000 = 256000. The caller allocates it (nonpaged pool in
 * the driver, heap in user mode) so this module stays allocation-free.
 * Returns 0 on error (err filled), >0 = key length on success. */
#define WL_LUKS_UNLOCK_SCRATCH_MAX (WL_LUKS_MAX_KEY_SIZE * WL_LUKS_MAX_STRIPES)

int winlvm_luks_unlock_keyslot(winlvm_byte_source *src,
                               const winlvm_luks_metadata *md,
                               int keyslot_index,
                               const char *passphrase, size_t passphrase_len,
                               uint8_t *volume_key, size_t volume_key_cap,
                               uint8_t *scratch, size_t scratch_cap,
                               winlvm_luks_unlock_err *err);

/* Verify a volume key against the stored digest (index 0). 1 = match. */
int winlvm_luks_verify_key(const winlvm_luks_metadata *md,
                           const uint8_t *volume_key, size_t key_len);

/* Decrypt a region of the encrypted segment. start_byte/len are offsets
 * into the SEGMENT (0 = first byte at segment data offset). buf must be
 * sector_size-aligned in length when len spans multiple sectors; a partial
 * trailing sector is handled via ciphertext stealing is NOT needed here —
 * LUKS2 data is sector-aligned by construction, so len must be a multiple
 * of segment sector_size. */
int winlvm_luks_decrypt_region(winlvm_byte_source *src,
                               const winlvm_luks_metadata *md,
                               const uint8_t *volume_key, size_t key_len,
                               uint64_t start_byte, uint8_t *buf, size_t len,
                               winlvm_luks_unlock_err *err);

/* Decrypting byte source: presents the LUKS2 payload (the PV) as a plain
 * readable device. Reads are decrypted on the fly, so the LVM2 layer can
 * parse the PV without knowing about the container. Reads must be
 * sector-aligned in length (use winlvm_luks_decrypt_view_read). */
typedef struct {
    winlvm_byte_source *backing;      /* the raw container image */
    const winlvm_luks_metadata *md;
    const uint8_t *volume_key;
    size_t key_len;
} winlvm_luks_view;

/* len must be a multiple of the segment sector size; offset may be
 * arbitrary. Returns bytes decrypted (= len) or 0 on error. */
size_t winlvm_luks_view_read(winlvm_luks_view *view,
                             uint64_t offset, void *buf, size_t len);

#endif
