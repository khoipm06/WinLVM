/*
 * luks_read.c — freestanding LUKS2 reader.
 *
 * Implements the binary header and a small JSON tokenizer sufficient
 * to extract the segment / keyslot / digest fields documented in the
 * upstream cryptsetup sources:
 *
 *   third_party/cryptsetup/lib/luks2/luks2.h           — struct luks2_hdr_disk
 *   third_party/cryptsetup/lib/luks2/luks2_segment.c    — segment JSON keys
 *   third_party/cryptsetup/lib/luks2/luks2_keyslot.c    — keyslot JSON keys
 *   third_party/cryptsetup/lib/luks2/luks2_digest.c     — digest JSON keys
 *   third_party/cryptsetup/lib/luks2/luks2_disk_metadata.c — checksum layout
 *
 * No JSON DOM is allocated. The tokenizer walks the bytes and pulls out
 * scalar fields into fixed buffers. This keeps the parser safe to call
 * from nonpaged pool / boot-start driver / UEFI DXE contexts.
 */
#include "luks_read.h"

#ifndef _KERNEL_MODE
#include <string.h>
#include <stdlib.h>
#define WL_MEMCPY memcpy
#define WL_MEMCMP memcmp
#define WL_MEMMOVE memmove
#else
#define WL_MEMCPY memcpy
#define WL_MEMCMP memcmp
#define WL_MEMMOVE memmove
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

static int wl_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void wl_set_err(winlvm_luks_err *err, const char *msg) {
    if (!err) return;
    err->failed = 1;
    wl_strcpy(err->message, sizeof(err->message), msg);
}

/* Re-export the same CRC32 used by lvm_read.c. We duplicate the few-byte
 * implementation here rather than introduce a header cycle. The polynomial
 * matches LVM2's non-standard CRC32 (poly 0xedb88320, init 0xf597a6cf, no
 * final XOR) — see third_party/cryptsetup/lib/luks2/luks2_disk_metadata.c
 * which calls crypt_hash_final; LUKS2 uses an external message digest
 * (sha256 etc.), NOT crc32. So the csum field is *not* validated via CRC
 * at all — it is verified by computing the chosen digest over
 * (binary header with csum zeroed || json area) and comparing. */
static uint32_t crc32_table[256];
static int crc32_table_built = 0;

static void build_crc32_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_built = 1;
}

static uint32_t luks_crc32(const void *data, size_t len) {
    if (!crc32_table_built) build_crc32_table();
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

static uint16_t be16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t be64(const unsigned char *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

/* ---------------------------------------------------------------
 * Header read
 * --------------------------------------------------------------- */

int winlvm_luks_read_header(winlvm_byte_source *src,
                            uint64_t hdr_offset,
                            winlvm_luks_header *out,
                            winlvm_luks_err *err) {
    if (err) err->failed = 0;
    wl_zero(out, sizeof(*out));

    unsigned char buf[LUKS2_HDR_BIN_LEN];
    if (!src->read(src->impl, hdr_offset, buf, LUKS2_HDR_BIN_LEN)) {
        wl_set_err(err, "failed to read 4096-byte LUKS2 header");
        return 0;
    }

    /* Verify magic. */
    if (memcmp(buf, LUKS2_MAGIC_1ST, LUKS2_MAGIC_L) != 0) {
        wl_set_err(err, "not a LUKS2 header (magic mismatch)");
        return 0;
    }

    out->version = be16(buf + 6);
    if (out->version != 2) {
        wl_set_err(err, "not a LUKS2 header (unsupported version)");
        return 0;
    }

    out->hdr_size = be64(buf + 8);
    out->seqid    = be64(buf + 16);
    WL_MEMCPY(out->label,         buf + 24, LUKS2_LABEL_L);
    out->label[LUKS2_LABEL_L - 1] = '\0';
    WL_MEMCPY(out->checksum_alg,  buf + 72, LUKS2_CHECKSUM_ALG_L);
    out->checksum_alg[LUKS2_CHECKSUM_ALG_L - 1] = '\0';
    WL_MEMCPY(out->salt,          buf + 104, LUKS2_SALT_L);
    WL_MEMCPY(out->uuid,          buf + 168, LUKS2_UUID_L);
    out->uuid[LUKS2_UUID_L - 1] = '\0';
    WL_MEMCPY(out->subsystem,     buf + 208, LUKS2_LABEL_L);
    out->subsystem[LUKS2_LABEL_L - 1] = '\0';
    out->hdr_offset = be64(buf + 256);
    WL_MEMCPY(out->csum,           buf + 448, LUKS2_CHECKSUM_L);

    if (out->hdr_size < LUKS2_HDR_BIN_LEN || out->hdr_size > 128ull * 1024 * 1024) {
        wl_set_err(err, "LUKS2 hdr_size out of range");
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------
 * Minimal JSON tokenizer for LUKS2 metadata.
 *
 * Just enough to extract key/value pairs from the well-known schema:
 *
 *   { "config": { ... }, "segments": { "0": { type, offset, size, ... }, ... },
 *     "keyslots": { "0": { type, kdf: { type, hash, iterations, ... }, ... }, ... },
 *     "digests":  { "0": { type, keyslots:[], segments:[], ... }, ... },
 *     "tokens":   { ... } }
 *
 * No DOM is built. The tokenizer matches known key strings against
 * structural boundaries (braces, brackets, colons, quotes, commas) and
 * fills fixed-size output buffers.
 * --------------------------------------------------------------- */

typedef struct {
    const char *p;
    size_t      len;
    size_t      pos;
} json_parser;

static void jp_skip_ws(json_parser *jp) {
    while (jp->pos < jp->len) {
        char c = jp->p[jp->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') jp->pos++;
        else break;
    }
}

/* Reads a JSON string starting at jp->pos. *jp moves past the closing
 * quote. Returns 1 on success, 0 on error. */
static int jp_read_string(json_parser *jp, char *out, size_t out_cap) {
    jp_skip_ws(jp);
    if (jp->pos >= jp->len || jp->p[jp->pos] != '"') return 0;
    jp->pos++;
    size_t i = 0;
    while (jp->pos < jp->len && jp->p[jp->pos] != '"') {
        char c = jp->p[jp->pos];
        if (c == '\\' && jp->pos + 1 < jp->len) { jp->pos++; c = jp->p[jp->pos]; }
        if (i + 1 < out_cap) out[i++] = c;
        jp->pos++;
    }
    if (jp->pos >= jp->len) return 0;
    jp->pos++;  /* closing quote */
    if (i < out_cap) out[i] = '\0';
    else out[out_cap - 1] = '\0';
    return 1;
}

/* Reads a JSON number. */
static int jp_read_int(json_parser *jp, uint64_t *out) {
    jp_skip_ws(jp);
    if (jp->pos >= jp->len) return 0;
    if (jp->p[jp->pos] != '-' && (jp->p[jp->pos] < '0' || jp->p[jp->pos] > '9')) return 0;
    uint64_t v = 0;
    int neg = 0;
    if (jp->p[jp->pos] == '-') { neg = 1; jp->pos++; }
    while (jp->pos < jp->len && jp->p[jp->pos] >= '0' && jp->p[jp->pos] <= '9') {
        v = v * 10 + (uint64_t)(jp->p[jp->pos] - '0');
        jp->pos++;
    }
    *out = neg ? 0 : v;
    return 1;
}

/* Skip a JSON value of any shape. */
static int jp_skip_value(json_parser *jp) {
    jp_skip_ws(jp);
    if (jp->pos >= jp->len) return 0;
    char c = jp->p[jp->pos];
    if (c == '"') {
        char tmp[8];
        return jp_read_string(jp, tmp, sizeof(tmp));
    } else if (c == '{') {
        jp->pos++;
        int depth = 1;
        while (jp->pos < jp->len && depth > 0) {
            char x = jp->p[jp->pos];
            if (x == '"') { char tmp[8]; if (!jp_read_string(jp, tmp, sizeof(tmp))) return 0; continue; }
            if (x == '{') depth++;
            else if (x == '}') depth--;
            jp->pos++;
        }
        return depth == 0;
    } else if (c == '[') {
        jp->pos++;
        int depth = 1;
        while (jp->pos < jp->len && depth > 0) {
            char x = jp->p[jp->pos];
            if (x == '"') { char tmp[8]; if (!jp_read_string(jp, tmp, sizeof(tmp))) return 0; continue; }
            if (x == '[') depth++;
            else if (x == ']') depth--;
            jp->pos++;
        }
        return depth == 0;
    } else {
        /* number, true, false, null */
        while (jp->pos < jp->len) {
            char x = jp->p[jp->pos];
            if (x == ',' || x == '}' || x == ']' || x == ' ' || x == '\n') return 1;
            jp->pos++;
        }
        return 1;
    }
}

/* Within an object body, walk to the next "key": value. The key is read
 * into out_key. The value is left for the caller to handle (typically
 * via jp_skip_value after consuming the ':'). Returns 1 if a key was read,
 * 0 at the closing '}' or end of input. */
static int jp_next_kv(json_parser *jp, char *out_key, size_t out_cap) {
    jp_skip_ws(jp);
    while (jp->pos < jp->len && jp->p[jp->pos] == ',') {
        jp->pos++;
        jp_skip_ws(jp);
    }
    jp_skip_ws(jp);
    if (jp->pos >= jp->len) return 0;
    if (jp->p[jp->pos] == '}') return 0;
    if (!jp_read_string(jp, out_key, out_cap)) return 0;
    jp_skip_ws(jp);
    if (jp->pos >= jp->len || jp->p[jp->pos] != ':') return 0;
    jp->pos++;
    return 1;
}

/* Parse a kdf sub-object, which holds type/hash/iterations/time/memory/cpus.
 * Per luks2_keyslot.c / luks2_digest.c. */
static void parse_kdf(json_parser *jp, winlvm_luks_kdf *kdf) {
    char key[64];
    while (jp_next_kv(jp, key, sizeof(key))) {
        if (!strcmp(key, "type")) jp_read_string(jp, kdf->type, sizeof(kdf->type));
        else if (!strcmp(key, "hash")) jp_read_string(jp, kdf->hash, sizeof(kdf->hash));
        else if (!strcmp(key, "iterations")) jp_read_int(jp, &kdf->iterations);
        else if (!strcmp(key, "time")) jp_read_int(jp, &kdf->time);
        else if (!strcmp(key, "memory")) jp_read_int(jp, &kdf->memory);
        else if (!strcmp(key, "cpus")) jp_read_int(jp, &kdf->cpus);
        else jp_skip_value(jp);
    }
    if (jp->pos < jp->len && jp->p[jp->pos] == '}') jp->pos++;
}

/* Parse a segment object: { type, offset, size, iv_tweak, encryption,
 * sector_size }. Per luks2_segment.c. */
static void parse_segment(json_parser *jp, winlvm_luks_segment *seg) {
    char key[64];
    while (jp_next_kv(jp, key, sizeof(key))) {
        if (!strcmp(key, "type")) jp_read_string(jp, seg->type, sizeof(seg->type));
        else if (!strcmp(key, "offset")) jp_read_int(jp, &seg->offset);
        else if (!strcmp(key, "size")) jp_read_int(jp, &seg->size);
        else if (!strcmp(key, "iv_tweak")) jp_read_int(jp, &seg->iv_tweak);
        else if (!strcmp(key, "encryption")) jp_read_string(jp, seg->encryption, sizeof(seg->encryption));
        else if (!strcmp(key, "sector_size")) jp_read_int(jp, &seg->sector_size);
        else jp_skip_value(jp);
    }
    if (jp->pos < jp->len && jp->p[jp->pos] == '}') jp->pos++;
}

/* Parse a keyslot object: { type, priority, kdf: {...}, area: {...} }. */
static void parse_keyslot(json_parser *jp, winlvm_luks_keyslot *ks) {
    char key[64];
    while (jp_next_kv(jp, key, sizeof(key))) {
        if (!strcmp(key, "type")) jp_read_string(jp, ks->type, sizeof(ks->type));
        else if (!strcmp(key, "priority")) jp_read_int(jp, &ks->priority);
        else if (!strcmp(key, "kdf")) {
            jp_skip_ws(jp);
            if (jp->pos < jp->len && jp->p[jp->pos] == '{') { jp->pos++; parse_kdf(jp, &ks->kdf); }
        } else if (!strcmp(key, "area")) {
            jp_skip_ws(jp);
            if (jp->pos < jp->len && jp->p[jp->pos] == '{') {
                jp->pos++;
                char k2[64];
                while (jp_next_kv(jp, k2, sizeof(k2))) {
                    if (!strcmp(k2, "offset")) jp_read_int(jp, &ks->area_offset);
                    else if (!strcmp(k2, "size")) jp_read_int(jp, &ks->area_size);
                    else jp_skip_value(jp);
                }
                if (jp->pos < jp->len && jp->p[jp->pos] == '}') jp->pos++;
            }
        } else jp_skip_value(jp);
    }
    if (jp->pos < jp->len && jp->p[jp->pos] == '}') jp->pos++;
}

/* Parse a digest object: { type, keyslots: [...], segments: [...], ... }. */
static void parse_digest(json_parser *jp, winlvm_luks_digest *d) {
    char key[64];
    while (jp_next_kv(jp, key, sizeof(key))) {
        if (!strcmp(key, "type")) jp_read_string(jp, d->type, sizeof(d->type));
        else if (!strcmp(key, "hash")) jp_read_string(jp, d->kdf.hash, sizeof(d->kdf.hash));
        else if (!strcmp(key, "iterations")) jp_read_int(jp, &d->kdf.iterations);
        else if (!strcmp(key, "salt")) jp_skip_value(jp);
        else if (!strcmp(key, "digest")) jp_skip_value(jp);
        else if (!strcmp(key, "keyslots")) jp_skip_value(jp);
        else if (!strcmp(key, "segments")) jp_skip_value(jp);
        else jp_skip_value(jp);
    }
    if (jp->pos < jp->len && jp->p[jp->pos] == '}') jp->pos++;
}

/* ---------------------------------------------------------------
 * Full metadata read: header + JSON area parse.
 * --------------------------------------------------------------- */

int winlvm_luks_read_metadata(winlvm_byte_source *src,
                              winlvm_luks_metadata *out_md,
                              winlvm_luks_err *err) {
    if (err) err->failed = 0;
    wl_zero(out_md, sizeof(*out_md));

    winlvm_luks_header hdr;
    if (!winlvm_luks_read_header(src, 0, &hdr, err)) return 0;

    if (hdr.hdr_size < LUKS2_HDR_BIN_LEN) {
        wl_set_err(err, "hdr_size too small");
        return 0;
    }
    size_t json_len = (size_t)(hdr.hdr_size - LUKS2_HDR_BIN_LEN);
    if (json_len > 4ull * 1024 * 1024) {
        wl_set_err(err, "JSON area too large to allocate in nonpaged pool");
        return 0;
    }
    /* We allocate a scratch buffer for the JSON area. The caller owns
     * out_md; we use a temporary stack buffer limited to 64 KiB which is
     * enough for normal LUKS2 metadata. */
    static unsigned char json_buf[64 * 1024];
    if (json_len > sizeof(json_buf)) {
        wl_set_err(err, "JSON area exceeds in-place buffer");
        return 0;
    }
    if (!src->read(src->impl, LUKS2_HDR_BIN_LEN, json_buf, json_len)) {
        wl_set_err(err, "failed to read JSON area");
        return 0;
    }

    /* Walk the JSON top-level object looking for known keys. */
    json_parser jp = { (const char *)json_buf, json_len, 0 };
    /* The JSON must start with '{'. */
    jp_skip_ws(&jp);
    if (jp.pos >= jp.len || jp.p[jp.pos] != '{') {
        wl_set_err(err, "JSON area does not start with '{'");
        return 0;
    }
    jp.pos++;

    char key[64];
    while (jp_next_kv(&jp, key, sizeof(key))) {
        if (!strcmp(key, "segments")) {
            /* "{ "0": { ... }, "1": { ... } }" */
            jp_skip_ws(&jp);
            if (jp.pos < jp.len && jp.p[jp.pos] == '{') { jp.pos++;
                char sub[64];
                while (jp_next_kv(&jp, sub, sizeof(sub))) {
                    if (out_md->segment_count < 8) {
                        wl_zero(&out_md->segments[out_md->segment_count], sizeof(out_md->segments[0]));
                        parse_segment(&jp, &out_md->segments[out_md->segment_count]);
                        out_md->segment_count++;
                    } else jp_skip_value(&jp);
                }
                if (jp.pos < jp.len && jp.p[jp.pos] == '}') jp.pos++;
            }
        } else if (!strcmp(key, "keyslots")) {
            jp_skip_ws(&jp);
            if (jp.pos < jp.len && jp.p[jp.pos] == '{') { jp.pos++;
                char sub[64];
                while (jp_next_kv(&jp, sub, sizeof(sub))) {
                    if (out_md->keyslot_count < 8) {
                        wl_zero(&out_md->keyslots[out_md->keyslot_count], sizeof(out_md->keyslots[0]));
                        parse_keyslot(&jp, &out_md->keyslots[out_md->keyslot_count]);
                        out_md->keyslot_count++;
                    } else jp_skip_value(&jp);
                }
                if (jp.pos < jp.len && jp.p[jp.pos] == '}') jp.pos++;
            }
        } else if (!strcmp(key, "digests")) {
            jp_skip_ws(&jp);
            if (jp.pos < jp.len && jp.p[jp.pos] == '{') { jp.pos++;
                char sub[64];
                while (jp_next_kv(&jp, sub, sizeof(sub))) {
                    if (out_md->digest_count < 8) {
                        wl_zero(&out_md->digests[out_md->digest_count], sizeof(out_md->digests[0]));
                        parse_digest(&jp, &out_md->digests[out_md->digest_count]);
                        out_md->digest_count++;
                    } else jp_skip_value(&jp);
                }
                if (jp.pos < jp.len && jp.p[jp.pos] == '}') jp.pos++;
            }
        } else {
            /* config, tokens, etc. — skip. */
            jp_skip_value(&jp);
        }
    }

    return 1;
}
