/*
 * luks_unlock.c — LUKS2 keyslot unlock + segment decrypt.
 *
 * AF_merge and diffuse are reimplemented from the public LUKS1 on-disk
 * format specification (also described by cryptsetup's GPL af.c, read as
 * a reference only, not copied):
 *
 *   diffuse(src, size): for each digest-size block i:
 *       hash_buf(block, digest_size) = SHA256(BE32(i) || block)
 *   AF_merge(src, blocksize, blocknumbers):
 *       buf = 0
 *       for i = 0 .. blocknumbers-2:  buf ^= src[i]; buf = diffuse(buf)
 *       dst = buf XOR src[last]
 */
#include "luks_unlock.h"
#include "../crypto/wl_sha256.h"
#include "../crypto/wl_aes_xts.h"

#ifndef _KERNEL_MODE
#include <string.h>
#define WL_MEMCPY memcpy
#define WL_MEMCMP memcmp
#else
#define WL_MEMCPY memcpy
#define WL_MEMCMP memcmp
#endif

static void set_uerr(winlvm_luks_unlock_err *err, const char *msg) {
    if (!err) return;
    err->failed = 1;
    size_t i = 0;
    while (msg[i] && i < sizeof(err->message) - 1) { err->message[i] = msg[i]; i++; }
    err->message[i] = '\0';
}

static int wl_streq2(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* base64 decode (standard alphabet, '='-terminated input tolerated) */
static size_t b64_decode(const char *in, uint8_t *out, size_t out_cap) {
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        char c = *in;
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_cap) return 0;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

/* LUKS1 diffuse: dst block i = SHA256(BE32(i) || src_block_i). */
static void af_diffuse(const uint8_t *src, uint8_t *dst, size_t blocksize) {
    wl_sha256_ctx h;
    uint8_t digest[32];
    size_t off = 0;
    uint32_t i = 0;
    while (off < blocksize) {
        size_t n = blocksize - off < 32 ? blocksize - off : 32;
        uint8_t iv[4] = { (uint8_t)(i >> 24), (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i };
        wl_sha256_init(&h);
        wl_sha256_update(&h, iv, 4);
        wl_sha256_update(&h, src + off, n);
        wl_sha256_final(&h, digest);
        WL_MEMCPY(dst + off, digest, n);
        off += n;
        i++;
    }
}

/* LUKS1 AF_merge (sha256 digest blocks). */
static int af_merge(const uint8_t *src, uint8_t *dst,
                    size_t blocksize, uint32_t blocknumbers) {
    uint8_t buf[WL_LUKS_MAX_KEY_SIZE];
    if (blocksize == 0 || blocksize > sizeof(buf) || blocknumbers == 0) return 0;

    for (size_t j = 0; j < blocksize; j++) buf[j] = 0;
    for (uint32_t i = 0; i + 1 < blocknumbers; i++) {
        const uint8_t *blk = src + (size_t)i * blocksize;
        for (size_t j = 0; j < blocksize; j++) buf[j] ^= blk[j];
        af_diffuse(buf, buf, blocksize);
    }
    const uint8_t *last = src + (size_t)(blocknumbers - 1) * blocksize;
    for (size_t j = 0; j < blocksize; j++) dst[j] = buf[j] ^ last[j];
    return 1;
}

/* plain64 tweak: LE64(sector) in the low 8 bytes of the 16-byte tweak. */
static void plain64_tweak(uint64_t sector, uint8_t tweak[16]) {
    for (int i = 0; i < 8; i++) tweak[i] = (uint8_t)(sector >> (8 * i));
    for (int i = 8; i < 16; i++) tweak[i] = 0;
}

int winlvm_luks_unlock_keyslot(winlvm_byte_source *src,
                               const winlvm_luks_metadata *md,
                               int keyslot_index,
                               const char *passphrase, size_t passphrase_len,
                               uint8_t *volume_key, size_t volume_key_cap,
                               uint8_t *scratch, size_t scratch_cap,
                               winlvm_luks_unlock_err *err) {
    if (keyslot_index < 0 || keyslot_index >= md->keyslot_count) {
        set_uerr(err, "keyslot index out of range");
        return 0;
    }
    const winlvm_luks_keyslot *ks = &md->keyslots[keyslot_index];

    if (!wl_streq2(ks->kdf.type, "pbkdf2")) {
        set_uerr(err, "unsupported keyslot KDF (pbkdf2 only)");
        return 0;
    }
    if (ks->key_size == 0 || ks->key_size > WL_LUKS_MAX_KEY_SIZE) {
        set_uerr(err, "keyslot key_size out of range");
        return 0;
    }
    if (ks->af_stripes == 0 || ks->af_stripes > WL_LUKS_MAX_STRIPES) {
        set_uerr(err, "keyslot stripe count out of range");
        return 0;
    }
    size_t need = (size_t)ks->key_size * (size_t)ks->af_stripes;
    if (need > ks->area_size) {
        set_uerr(err, "keyslot area too small for AF stripes");
        return 0;
    }
    if (volume_key_cap < ks->key_size) {
        set_uerr(err, "volume_key buffer too small");
        return 0;
    }
    if (ks->key_size != 64) {
        set_uerr(err, "expected a 64-byte aes-xts-plain64 keyslot key");
        return 0;
    }
    if (scratch_cap < need) {
        set_uerr(err, "scratch buffer too small");
        return 0;
    }

    /* 1. area key from the passphrase */
    uint8_t kdf_salt[64];
    size_t salt_len = b64_decode(ks->kdf_salt_b64, kdf_salt, sizeof(kdf_salt));
    if (salt_len == 0) { set_uerr(err, "invalid KDF salt"); return 0; }

    uint8_t area_key[WL_LUKS_MAX_KEY_SIZE];
    wl_pbkdf2_sha256(passphrase, passphrase_len,
                     kdf_salt, salt_len,
                     (uint32_t)ks->kdf.iterations,
                     area_key, (size_t)ks->key_size);

    /* 2. decrypt the keyslot area: XTS with the area key, plain64 sector
     *    numbers starting at 0 at area_offset. The XTS key size equals the
     *    keyslot key size (e.g. 32 bytes => AES-128-XTS for a 16-byte area
     *    key never occurs; cryptsetup uses key_size/2 AES with full XTS —
     *    for key_size 64 the area key IS the 64-byte XTS key). */
    if (ks->key_size != 64) { set_uerr(err, "expected aes-xts 64-byte key"); return 0; }
    wl_xts_ctx xts;
    wl_xts_init(&xts, area_key, 64);

    uint8_t *area_buf = scratch;
    size_t sect = (need + 511) / 512;

    uint8_t sector[512];
    size_t have = 0;
    for (size_t s = 0; s < sect; s++) {
        if (!src->read(src->impl, ks->area_offset + s * 512, sector, 512)) {
            set_uerr(err, "failed to read keyslot area");
            return 0;
        }
        uint8_t tweak[16];
        plain64_tweak(s, tweak);
        wl_xts_crypt(&xts, tweak, 0, sector, 512);
        size_t take = need - have < 512 ? need - have : 512;
        WL_MEMCPY(area_buf + have, sector, take);
        have += take;
    }
    if (have < need) { set_uerr(err, "keyslot area truncated"); return 0; }

    /* 3. AF-merge to the volume key */
    if (!af_merge(area_buf, volume_key, (size_t)ks->key_size,
                  (uint32_t)ks->af_stripes)) {
        set_uerr(err, "AF merge failed");
        return 0;
    }
    return (int)ks->key_size;
}

int winlvm_luks_verify_key(const winlvm_luks_metadata *md,
                           const uint8_t *volume_key, size_t key_len) {
    if (md->digest_count < 1) return 0;
    const winlvm_luks_digest *d = &md->digests[0];
    if (!wl_streq2(d->type, "pbkdf2")) return 0;

    uint8_t salt[64], stored[64], computed[32];
    size_t salt_len = b64_decode(d->salt_b64, salt, sizeof(salt));
    size_t stored_len = b64_decode(d->digest_b64, stored, sizeof(stored));
    if (salt_len == 0 || stored_len != 32) return 0;

    wl_pbkdf2_sha256(volume_key, key_len, salt, salt_len,
                     (uint32_t)d->kdf.iterations, computed, 32);
    return WL_MEMCMP(computed, stored, 32) == 0;
}

int winlvm_luks_decrypt_region(winlvm_byte_source *src,
                               const winlvm_luks_metadata *md,
                               const uint8_t *volume_key, size_t key_len,
                               uint64_t start_byte, uint8_t *buf, size_t len,
                               winlvm_luks_unlock_err *err) {
    if (md->segment_count < 1) { set_uerr(err, "no segments"); return 0; }
    const winlvm_luks_segment *seg = &md->segments[0];
    uint64_t sector_size = seg->sector_size ? seg->sector_size : 512;
    if (sector_size != 512 && sector_size != 4096) {
        set_uerr(err, "unsupported segment sector size");
        return 0;
    }
    if (len % sector_size != 0) { set_uerr(err, "length not sector aligned"); return 0; }
    if (!wl_streq2(seg->encryption, "aes-xts-plain64")) {
        set_uerr(err, "unsupported encryption mode");
        return 0;
    }
    if (key_len != 64) { set_uerr(err, "XTS-AES-256 needs a 64-byte key"); return 0; }

    wl_xts_ctx xts;
    wl_xts_init(&xts, volume_key, 64);

    uint64_t first_sector = start_byte / sector_size;
    for (uint64_t i = 0; i < len / sector_size; i++) {
        uint8_t tweak[16];
        plain64_tweak(first_sector + i + seg->iv_tweak, tweak);
        uint8_t *dst = buf + (size_t)(i * sector_size);
        if (!src->read(src->impl, seg->offset + start_byte + i * sector_size,
                       dst, (size_t)sector_size)) {
            set_uerr(err, "failed to read segment data");
            return 0;
        }
        wl_xts_crypt(&xts, tweak, 0, dst, (size_t)sector_size);
    }
    return 1;
}

size_t winlvm_luks_view_read(winlvm_luks_view *view,
                             uint64_t offset, void *buf, size_t len) {
    /* Decompose the caller's window into whole sectors, decrypt, copy back.
     * The segment sector size is 512 in practice; treat the payload device
     * as 512-granular (LUKS2 with 4K crypto sectors would need a 4K-aware
     * caller, which the driver handles by reading whole sectors anyway). */
    if (len == 0 || !view || !view->backing) return 0;
    const winlvm_luks_segment *seg = &view->md->segments[0];
    uint64_t ss = seg->sector_size ? seg->sector_size : 512;

    uint64_t first = (offset / ss) * ss;
    uint64_t covered_end = ((offset + len + ss - 1) / ss) * ss;
    if (covered_end - first > (4ull << 20)) return 0;  /* 4 MiB sanity cap */

    uint8_t sector[4096];
    if (ss > sizeof(sector)) return 0;
    uint8_t *out = (uint8_t *)buf;
    uint64_t taken = 0;

    wl_xts_ctx xts;
    wl_xts_init(&xts, view->volume_key, view->key_len);

    for (uint64_t base = first; base < covered_end; base += ss) {
        uint64_t secno = base / ss;
        uint8_t tweak[16];
        plain64_tweak(secno + seg->iv_tweak, tweak);
        if (!view->backing->read(view->backing->impl, seg->offset + base,
                                 sector, (size_t)ss)) return 0;
        wl_xts_crypt(&xts, tweak, 0, sector, (size_t)ss);
        /* copy the part of this sector the caller asked for */
        uint64_t want_from = offset > base ? offset : base;
        uint64_t want_to = offset + len < base + ss ? offset + len : base + ss;
        if (want_from < base + ss && want_to > base) {
            uint64_t skip = want_from - base;
            uint64_t n = want_to - want_from;
            WL_MEMCPY(out + taken, sector + skip, (size_t)n);
            taken += n;
        }
    }
    return (size_t)taken;
}
