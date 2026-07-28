/*
 * luks_read.c — freestanding LUKS2 reader.
 */
#include "luks_read.h"

#ifndef _KERNEL_MODE
#include <string.h>
#define WL_MEMCPY memcpy
#define WL_MEMCMP memcmp
#else
#define WL_MEMCPY memcpy
#define WL_MEMCMP memcmp
#endif

static void wl_zero(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static void wl_strcpy(char *dst, size_t dst_cap, const char *src) {
    size_t i = 0;
    if (dst_cap == 0) return;
    while (src[i] && i + 1 < dst_cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void wl_set_err(winlvm_luks_err *err, const char *msg) {
    if (!err) return;
    err->failed = 1;
    wl_strcpy(err->message, sizeof(err->message), msg);
}

static uint16_t le16(const unsigned char *p) {
    /* LUKS uses Big-Endian, not Little-Endian! Wait.
     * Spec says: "All fields are in big-endian byte order." */
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const unsigned char *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

int winlvm_luks_read_header(const void *buf, size_t bufsz,
                            winlvm_luks_header *out,
                            winlvm_luks_err *err) {
    if (err) err->failed = 0;
    wl_zero(out, sizeof(*out));

    if (bufsz < 4096) {
        wl_set_err(err, "buffer too small for LUKS2 header");
        return 0;
    }

    const unsigned char *p = (const unsigned char *)buf;
    const char magic[6] = { 'L','U','K','S', (char)(unsigned char)0xba, (char)(unsigned char)0xbe };

    if (WL_MEMCMP(p, magic, 6) != 0) {
        wl_set_err(err, "not a LUKS header (magic mismatch)");
        return 0;
    }

    out->version = le16(p + 6);
    if (out->version != 2) {
        wl_set_err(err, "not a LUKS2 header (unsupported version)");
        return 0;
    }

    out->hdr_size = be64(p + 8);
    out->seqid    = be64(p + 16);
    WL_MEMCPY(out->label, p + 24, 48);
    out->label[47] = '\0';
    WL_MEMCPY(out->checksum_alg, p + 72, 32);
    out->checksum_alg[31] = '\0';
    WL_MEMCPY(out->salt, p + 104, 64);
    WL_MEMCPY(out->uuid, p + 168, 40);
    out->uuid[39] = '\0';
    WL_MEMCPY(out->subsystem, p + 208, 48);
    out->subsystem[47] = '\0';
    out->hdr_offset = be64(p + 256);
    WL_MEMCPY(out->csum, p + 448, 64);

    return 1;
}
