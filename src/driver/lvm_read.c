/*
 * lvm_read.c — freestanding LVM2 read path. See lvm_read.h for the why.
 *
 * This file MUST compile clean under both:
 *   - the WDK kernel-mode build (/kernel, no UCRT), and
 *   - a user-mode C test harness built with gcc/clang (for the corpus
 *     oracle tests).
 *
 * It uses only <stddef.h> <stdint.h> and the above types. The only libc-ish
 * helpers it needs are byte comparisons and integer ops; memcpy/memcmp are
 * provided as static loops so no CRT linkage is required (the WDK build runs
 * with /NODEFAULTLIB, and memcpy inside ntoskrnl is reachable but we keep
 * this module self-contained for testability). The kernel build will let
 * the compiler's intrinsics resolve them to ntoskrnl's exports; that's fine.
 */
#include "lvm_read.h"

#ifndef _KERNEL_MODE
/* User-mode test build: use real mem*. */
#include <string.h>
#define WL_MEMCPY memcpy
#define WL_MEMCMP memcmp
#else
/* Kernel build: ntoskrnl provides these as well-known exports; let
 * intrinsics/lowering find them. */
#define WL_MEMCPY memcpy
#define WL_MEMCMP memcmp
#endif

/* ------------------------------------------------------------------ */
/* Small helpers (no CRT dependency for what's testable in isolation).  */
/* ------------------------------------------------------------------ */

static void wl_zero(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static int wl_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void wl_strcpy(char *dst, size_t dst_cap, const char *src) {
    size_t i = 0;
    if (dst_cap == 0) return;
    while (src[i] && i + 1 < dst_cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void wl_set_err(winlvm_lvm_err *err, const char *msg) {
    if (!err) return;
    err->failed = 1;
    wl_strcpy(err->message, sizeof(err->message), msg);
}

/* ------------------------------------------------------------------ */
/* CRC32 — LVM variant. Compile-time same constant as lvmfmt/crc32.cpp.
 * We build the table at first call (the kernel C runtime's static-init
 * model is fine; we guard with a flag rather than assuming init order).   */
/* ------------------------------------------------------------------ */

static uint32_t crc_table[256];
static int crc_table_built = 0;

static void build_crc_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_built = 1;
}

uint32_t winlvm_lvm_crc32(const void *data, size_t len) {
    if (!crc_table_built) build_crc_table();
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xf597a6cfu;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;  /* no final XOR. matches lvmfmt. */
}

/* ------------------------------------------------------------------ */
/* Byte source reads, with short-read safety.                           */
/* ------------------------------------------------------------------ */

static int src_read_full(winlvm_byte_source *src, uint64_t off,
                         void *buf, size_t len) {
    /* Wrap-checked: `off + len` alone can rotate past src->size on
     * adversarial on-disk numbers, and the (0,0)-terminated area lists
     * hand us attacker-chosen offsets before any validation. */
    if (off > src->size || len > src->size - off) return 0;
    size_t got = src->read(src->impl, off, buf, len);
    return got == len;
}

/* ------------------------------------------------------------------ */
/* Little-endian readers.                                              */
/* ------------------------------------------------------------------ */

static uint16_t le16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const unsigned char *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* ------------------------------------------------------------------ */
/* 1. Label scan.                                                       */
/* ------------------------------------------------------------------ */

static const char LABEL_MAGIC[8] = { 'L','A','B','E','L','O','N','E' };
static const char LABEL_TYPE[8]  = { 'L','V','M','2',' ','0','0','1' };

/* Parse a null-terminated area list at cursor; advancing past (0,0). */
static int parse_area_list(const unsigned char *buf, size_t bufsz, size_t *cursor,
                           winlvm_disk_area *out, winlvm_lvm_err *err) {
    size_t i = 0;
    for (;;) {
        if (*cursor + 16 > bufsz) { wl_set_err(err, "PV header truncated in disk-area list"); return 0; }
        uint64_t off = le64(buf + *cursor);
        uint64_t sz  = le64(buf + *cursor + 8);
        *cursor += 16;
        if (off == 0 && sz == 0) { out[i].offset = 0; out[i].size = 0; return 1; }  /* sentinel */
        if (i >= WINLVM_MAX_MD_AREAS) { wl_set_err(err, "PV header declares too many disk areas"); return 0; }
        out[i].offset = off;
        out[i].size = sz;
        i++;
    }
}

/* Find the LVM2 label in the first WINLVM_LABEL_SCAN_BYTES. The LVH is
 * 512 bytes and itself begins with LABELONE. We scan sector-by-sector. */
static int read_pv_label(winlvm_byte_source *src, winlvm_pv_label *out,
                         winlvm_lvm_err *err) {
    unsigned char head[WINLVM_LABEL_SCAN_BYTES];
    if (!src_read_full(src, 0, head, sizeof(head))) {
        wl_set_err(err, "failed reading device head for label scan");
        return 0;
    }

    for (size_t off = 0; off + 512 <= sizeof(head); off += 512) {
        const unsigned char *p = head + off;
        if (WL_MEMCMP(p, LABEL_MAGIC, 8) != 0) continue;

        uint32_t stored = le32(p + 16);
        uint32_t crc = winlvm_lvm_crc32(p + 20, 512 - 20);
        if (stored != crc) { wl_set_err(err, "PV label CRC mismatch"); return 0; }
        if (WL_MEMCMP(p + 24, LABEL_TYPE, 8) != 0) { wl_set_err(err, "unsupported PV label type"); return 0; }

        uint32_t data_off = le32(p + 20);
        if (data_off < 32 || data_off >= 512) { wl_set_err(err, "PV label data offset out of range"); return 0; }

        /* The PV header lives within the same 512-byte sector. */
        const unsigned char *pvh = p + data_off;
        /* Copy raw PV UUID (32 chars) + device_size. */
        WL_MEMCPY(out->pv_uuid, pvh, 32);
        out->pv_uuid[32] = '\0';
        out->device_size = le64(pvh + 32);

        size_t cursor = 40;
        if (!parse_area_list(pvh, 512 - data_off, &cursor, out->data_areas, err)) return 0;
        if (!parse_area_list(pvh, 512 - data_off, &cursor, out->metadata_areas, err)) return 0;
        return 1;
    }
    wl_set_err(err, "no LVM2 PV label found in first 8 KiB");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 2. mda_header parse + CRC.                                          */
/* ------------------------------------------------------------------ */

static const char MDA_MAGIC[16] = { ' ','L','V','M','2',' ','x','[',
                                    '5','A','%','r','0','N','*','>' };
#define MDA_HEADER_SIZE 512u

static int read_mda_header(winlvm_byte_source *src, const winlvm_disk_area *area,
                            winlvm_mda_header *out, winlvm_lvm_err *err) {
    if (area->size < MDA_HEADER_SIZE ||
        area->offset > src->size ||
        area->size > src->size - area->offset) {
        wl_set_err(err, "metadata area lies outside the device");
        return 0;
    }
    unsigned char buf[MDA_HEADER_SIZE];
    if (!src_read_full(src, area->offset, buf, sizeof(buf))) {
        wl_set_err(err, "failed reading mda_header");
        return 0;
    }
    uint32_t stored = le32(buf);
    uint32_t crc = winlvm_lvm_crc32(buf + 4, sizeof(buf) - 4);
    if (stored != crc) { wl_set_err(err, "mda_header CRC mismatch"); return 0; }
    if (WL_MEMCMP(buf + 4, MDA_MAGIC, sizeof(MDA_MAGIC)) != 0) { wl_set_err(err, "mda_header magic mismatch"); return 0; }
    if (le32(buf + 20) != 1) { wl_set_err(err, "unsupported mda_header version"); return 0; }

    /* raw_locn[0] == committed metadata.
     * Per the LVM2 on-disk format, raw_locn.offset is RELATIVE to the
     * metadata area start (mda.start), not an absolute disk offset. */
    out->commit_offset = le64(buf + 40);  /* offset relative to mda.start */
    out->commit_size   = le64(buf + 48);
    out->commit_checksum = le32(buf + 56);

    if (out->commit_size == 0) { wl_set_err(err, "metadata area contains no committed metadata"); return 0; }
    if (out->commit_size > WINLVM_MAX_METADATA_BYTES) { wl_set_err(err, "committed metadata descriptor is out of range"); return 0; }
    if (out->commit_offset < MDA_HEADER_SIZE ||
        out->commit_offset >= area->size) { wl_set_err(err, "committed metadata offset is inside the header or past the area"); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 3. Ring-buffer text extraction + CRC + NUL handling.                */
/* ------------------------------------------------------------------ */

/* Caller supplies a buffer of at least WINLVM_MAX_METADATA_BYTES. */
static int read_metadata_text(winlvm_byte_source *src, const winlvm_disk_area *area,
                              const winlvm_mda_header *mda, char *text_out,
                              size_t *text_len, winlvm_lvm_err *err) {
    /* The ring buffer occupies [area->offset + 512, area->offset + area->size).
     * A record that runs off the end wraps back to the start of that region.
     * mda->commit_offset is relative to area->offset (the metadata area start). */
    uint64_t ring_begin = area->offset + MDA_HEADER_SIZE;
    uint64_t ring_size  = area->size - MDA_HEADER_SIZE;
    uint64_t abs_commit_offset = area->offset + mda->commit_offset;
    uint64_t rel = mda->commit_offset - MDA_HEADER_SIZE;
    if (mda->commit_size > ring_size) { wl_set_err(err, "metadata larger than the metadata area"); return 0; }
    uint64_t first = (mda->commit_size < ring_size - rel) ? mda->commit_size : (ring_size - rel);
    uint64_t wrapped = mda->commit_size - first;

    char *buf = text_out;
    if (!src_read_full(src, abs_commit_offset, buf, (size_t)first)) {
        wl_set_err(err, "failed reading metadata text"); return 0;
    }
    if (wrapped &&
        !src_read_full(src, ring_begin, buf + first, (size_t)wrapped)) {
        wl_set_err(err, "failed reading wrapped metadata text"); return 0;
    }

    uint32_t crc = winlvm_lvm_crc32(buf, (size_t)mda->commit_size);
    if (crc != mda->commit_checksum) { wl_set_err(err, "metadata text CRC mismatch"); return 0; }

    /* LVM's commit_size includes a trailing NUL that vgcfgbackup files omit;
     * our parser must treat it as end-of-text (see the Phase 1 fix in
     * src/lvmfmt/config.cpp). */
    size_t len = (size_t)mda->commit_size;
    buf[len] = '\0';  /* make sure the region is NUL-terminated regardless */
    /* Also drop the trailing NUL terminator from the reported length so the
     * parser's trivia loop terminates naturally on the last real byte. */
    while (len > 0 && buf[len - 1] == '\0') len--;
    *text_len = len;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 4. Restricted text-config parser.                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    size_t len;
    size_t pos;
} parser;

static void p_init(parser *ps, const char *s, size_t n) { ps->p = s; ps->len = n; ps->pos = 0; }

static int p_eof(const parser *ps) { return ps->pos >= ps->len; }

static int is_ident_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           c == '_' || c == '-' || c == '.' || c == '+' || c == '/';
}

/* Skip whitespace and #comments and, critically, NULs (see Phase 1 bug). */
static void p_skip(parser *ps) {
    while (ps->pos < ps->len) {
        char c = ps->p[ps->pos];
        if (c == '#') { while (ps->pos < ps->len && ps->p[ps->pos] != '\n') ps->pos++; }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0') ps->pos++;
        else break;
    }
}

/* memmove forward declaration — kernel exports this; user mode has it. */
static void wl_memmove(void *dst, const void *src, size_t n);

static int p_ident(parser *ps, char *out, size_t cap) {
    p_skip(ps);
    size_t start = ps->pos;
    while (ps->pos < ps->len && is_ident_char(ps->p[ps->pos])) ps->pos++;
    if (ps->pos == start) return 0;
    size_t n = ps->pos - start;
    if (n >= cap) n = cap - 1;
    wl_memmove(out, ps->p + start, n);
    out[n] = '\0';
    return 1;
}

/* Strip dashes in-place from UUIDs. */
static void wl_strip_dashes(char *s) {
    char *d = s;
    while (*s) {
        if (*s != '-') *d++ = *s;
        s++;
    }
    *d = '\0';
}

static void wl_memmove(void *dst, const void *src, size_t n) {
    char *d = (char *)dst; const char *s = (const char *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
}

static int p_string(parser *ps, char *out, size_t cap) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '"') return 0;
    ps->pos++;
    size_t i = 0;
    while (ps->pos < ps->len && ps->p[ps->pos] != '"') {
        if (ps->p[ps->pos] == '\\' && ps->pos + 1 < ps->len) ps->pos++;  /* skip escape */
        if (i + 1 < cap) out[i++] = ps->p[ps->pos];
        if (ps->pos < ps->len) ps->pos++;
    }
    if (ps->pos >= ps->len) return 0;
    ps->pos++;  /* closing quote */
    out[i] = '\0';
    return 1;
}

static int p_int(parser *ps, uint64_t *out) {
    p_skip(ps);
    int neg = 0;
    if (ps->pos < ps->len && (ps->p[ps->pos] == '-' || ps->p[ps->pos] == '+')) {
        neg = (ps->p[ps->pos] == '-');
        ps->pos++;
    }
    if (ps->pos >= ps->len || ps->p[ps->pos] < '0' || ps->p[ps->pos] > '9') return 0;
    /* Saturate at INT64_MAX like lvmfmt's strtoll, instead of wrapping
     * mod 2^64 — the two parsers must agree on adversarial metadata
     * (contract: lvm_read.h "MUST match the C++ lvmfmt library"). */
    uint64_t v = 0;
    while (ps->pos < ps->len && ps->p[ps->pos] >= '0' && ps->p[ps->pos] <= '9') {
        uint64_t digit = (uint64_t)(ps->p[ps->pos] - '0');
        if (v > ((uint64_t)INT64_MAX - digit) / 10) { v = (uint64_t)INT64_MAX; }
        else v = v * 10 + digit;
        if (v == (uint64_t)INT64_MAX) {
            /* drain remaining digits so the cursor lands past the number */
            while (ps->pos < ps->len && ps->p[ps->pos] >= '0' && ps->p[ps->pos] <= '9') ps->pos++;
            break;
        }
        ps->pos++;
    }
    *out = neg ? 0 : v;
    return 1;
}

/* Skip the entire bracketed body of a section whose opening '{' is the next
 * non-trivia char. Used to fast-skip sections we don't care about (preamble
 * entries, hidden sub-VGs, etc.). */
static void p_skip_section(parser *ps) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '{') return;
    ps->pos++;
    int depth = 1;
    while (ps->pos < ps->len && depth > 0) {
        char c = ps->p[ps->pos];
        if (c == '{') depth++;
        else if (c == '}') depth--;
        ps->pos++;
    }
}

/* Parse a stripes = [ "pv0", 0, "pv1", 0 ] array into seg->stripes. */
static void p_stripes(parser *ps, winlvm_segment *seg) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '[') return;
    ps->pos++;
    size_t i = 0;
    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len) return;
        char c = ps->p[ps->pos];
        if (c == ']') { ps->pos++; break; }
        if (c == ',') { ps->pos++; continue; }
        if (c == '"') {
            if (i < (sizeof(seg->stripes) / sizeof(seg->stripes[0]))) {
                char pvkey[64];
                if (p_string(ps, pvkey, sizeof(pvkey))) {
                    wl_strcpy(seg->stripes[i].pv_key, sizeof(seg->stripes[i].pv_key), pvkey);
                }
                p_skip(ps);
                if (ps->pos < ps->len && ps->p[ps->pos] == ',') ps->pos++;
                uint64_t end_ext = 0;
                if (p_int(ps, &end_ext)) {
                    seg->stripes[i].start_extent = end_ext;
                }
                i++;
            } else {
                seg->stripe_overflow = 1;
                /* drain the rest */
                while (ps->pos < ps->len && ps->p[ps->pos] != ']') ps->pos++;
                if (ps->pos < ps->len) ps->pos++;
                break;
            }
        } else {
            ps->pos++;
        }
    }
    seg->stripe_count_used = i;
}

static void parse_segment(parser *ps, winlvm_segment *seg) {
    /* Already consumed the 'segmentN' identifier; cursor is at '{'. */
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '{') return;
    ps->pos++;
    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') { if (ps->pos < ps->len) ps->pos++; return; }
        char key[64];
        if (!p_ident(ps, key, sizeof(key))) { ps->pos++; continue; }
        p_skip(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == '=') {
            ps->pos++;
            p_skip(ps);
            if (wl_streq(key, "start_extent")) p_int(ps, &seg->start_extent);
            else if (wl_streq(key, "extent_count")) p_int(ps, &seg->extent_count);
            else if (wl_streq(key, "stripe_count")) p_int(ps, &seg->stripe_count);
            else if (wl_streq(key, "stripe_size")) { uint64_t v; p_int(ps, &v); }
            else if (wl_streq(key, "origin")) p_string(ps, seg->origin, sizeof(seg->origin));
            else if (wl_streq(key, "type")) {
                /* On-disk LVM2 metadata writes segment types as quoted
                 * strings ("thin", "raid1", "striped", ...). Accept a bare
                 * identifier too for vgcfgbackup-style text. Fail closed:
                 * anything unrecognized stays WINLVM_SEG_OTHER and the LV
                 * is refused rather than mapped as linear. */
                char t[32];
                if (p_string(ps, t, sizeof(t)) || p_ident(ps, t, sizeof(t))) {
                    if (wl_streq(t, "striped") || wl_streq(t, "linear"))
                        seg->type = WINLVM_SEG_STRIPED;
                    else
                        seg->type = WINLVM_SEG_OTHER;
                }
            }
            else if (wl_streq(key, "stripes")) {
                p_stripes(ps, seg);
            }
            else {
                /* skip a single value or array */
                if (ps->pos < ps->len && ps->p[ps->pos] == '[') {
                    int depth = 0;
                    while (ps->pos < ps->len) {
                        char c = ps->p[ps->pos];
                        if (c == '[') depth++;
                        else if (c == ']') { depth--; if (depth == 0) { ps->pos++; break; } }
                        else if (c == '{') p_skip_section(ps);
                        ps->pos++;
                        if (depth == 0 && c != '[') break;
                    }
                } else {
                    while (ps->pos < ps->len && ps->p[ps->pos] != '\n') ps->pos++;
                }
            }
        } else if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            p_skip_section(ps);
        } else {
            ps->pos++;
        }
    }
}

/* Inside logical_volumes { lvname { ... } lvname2 { ... } } parser. */
static void parse_lv_body(parser *ps, winlvm_logical_volume *lv) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '{') return;
    ps->pos++;
    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') { if (ps->pos < ps->len) ps->pos++; return; }
        char key[64];
        if (!p_ident(ps, key, sizeof(key))) { ps->pos++; continue; }
        p_skip(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == '=') {
            ps->pos++;
            p_skip(ps);
            if (wl_streq(key, "segment_count")) { uint64_t v; p_int(ps, &v); lv->declared_segment_count = v; }
            else if (wl_streq(key, "status")) {
                /* status = ["READ", "WRITE", "VISIBLE"] */
                if (ps->pos < ps->len && ps->p[ps->pos] == '[') {
                    ps->pos++;
                    for (;;) {
                        p_skip(ps);
                        if (ps->pos >= ps->len || ps->p[ps->pos] == ']') { if (ps->pos < ps->len) ps->pos++; break; }
                        if (ps->p[ps->pos] == ',') { ps->pos++; continue; }
                        char s[32];
                        if (p_string(ps, s, sizeof(s))) {
                            if (wl_streq(s, "VISIBLE")) lv->visible = 1;
                        } else { ps->pos++; }
                    }
                }
            }
            else if (wl_streq(key, "id"))   { char v[64]; if (p_string(ps, v, sizeof(v))) { wl_strip_dashes(v); wl_strcpy(lv->id, sizeof(lv->id), v); } }
            else {
                /* skip a value or array */
                if (ps->pos < ps->len && ps->p[ps->pos] == '[') {
                    int depth = 0;
                    while (ps->pos < ps->len) {
                        char c = ps->p[ps->pos];
                        if (c == '[') depth++;
                        else if (c == ']') { depth--; if (depth == 0) { ps->pos++; break; } }
                        ps->pos++;
                    }
                } else {
                    while (ps->pos < ps->len && ps->p[ps->pos] != '\n') ps->pos++;
                }
            }
        } else if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            /* a segment: name is already in 'key', body is this {...} */
            if (wl_streq(key, "segment")) { /* segmentN */ }  /* names start with 'segment' */
            if (lv->segment_count < (sizeof(lv->segments) / sizeof(lv->segments[0]))) {
                winlvm_segment *seg = &lv->segments[lv->segment_count];
                wl_zero(seg, sizeof(*seg));
                /* Fail closed: an unreadable or missing type key must leave
                 * the segment unmappable, never silently linear. */
                seg->type = WINLVM_SEG_OTHER;
                seg->stripe_count = 1;
                parse_segment(ps, seg);
                lv->segment_count++;
            } else {
                lv->segment_overflow = 1;
                p_skip_section(ps);
            }
        } else {
            ps->pos++;
        }
    }
}

/* Inside physical_volumes { pv0 { ... } pv1 { ... } }. */
static void parse_pv_body(parser *ps, winlvm_physical_volume *pv) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '{') return;
    ps->pos++;
    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') { if (ps->pos < ps->len) ps->pos++; return; }
        char key[64];
        if (!p_ident(ps, key, sizeof(key))) { ps->pos++; continue; }
        p_skip(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == '=') {
            ps->pos++;
            p_skip(ps);
            if (wl_streq(key, "id")) { char v[64]; if (p_string(ps, v, sizeof(v))) { wl_strip_dashes(v); wl_strcpy(pv->id, sizeof(pv->id), v); } }
            else if (wl_streq(key, "device"))  p_string(ps, pv->device, sizeof(pv->device));
            else if (wl_streq(key, "pe_start")) p_int(ps, &pv->pe_start);
            else if (wl_streq(key, "pe_count")) p_int(ps, &pv->pe_count);
            else {
                if (ps->pos < ps->len && ps->p[ps->pos] == '[') {
                    int depth = 0;
                    while (ps->pos < ps->len) {
                        char c = ps->p[ps->pos];
                        if (c == '[') depth++;
                        else if (c == ']') { depth--; if (depth == 0) { ps->pos++; break; } }
                        ps->pos++;
                    }
                } else {
                    while (ps->pos < ps->len && ps->p[ps->pos] != '\n') ps->pos++;
                }
            }
        } else if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            p_skip_section(ps);
        } else {
            ps->pos++;
        }
    }
}

/* Parse the top-level VG block. The on-disk raw format has the body
 * (vgname { ... }) preceded or followed by the preamble entries; winmd-style
 * scans for the section whose body contains 'id' and 'extent_size'. Here we
 * parse any top-level section, looking for the one with both an id= and an
 * extent_size= member inside. */
static int parse_vg(const char *text, size_t text_len, winlvm_volume_group *vg,
                    winlvm_pv_label *label, char *md_text,
                    size_t *parsed_vg /* bitmap of which lv supported */);

static int parse_vg(const char *text, size_t text_len, winlvm_volume_group *vg,
                    winlvm_pv_label *label, char *md_text,
                    size_t *parsed_vg) {
    (void)md_text; (void)parsed_vg;
    /* Zero the caller's vg directly so writes during parsing are well-defined. */
    wl_zero(vg, sizeof(*vg));
    parser ps; p_init(&ps, text, text_len);
    int found_vg = 0;
    while (!p_eof(&ps)) {
        p_skip(&ps);
        if (p_eof(&ps)) break;
        char sec_name[64];
        if (!p_ident(&ps, sec_name, sizeof(sec_name))) { ps.pos++; continue; }
        p_skip(&ps);
        if (ps.pos < ps.len && ps.p[ps.pos] == '{') {
            ps.pos++;
            /* tentatively mark this as the VG if it has id= and extent_size= */
            wl_strcpy(vg->name, sizeof(vg->name), sec_name);

            /* slow scan: we use a nested parser only to detect id+extent_size
             * at the top of this section body. Simpler: walk the whole body
             * looking for known keys. */
            size_t body_start = ps.pos;
            int depth = 1;
            int has_id = 0, has_extent = 0;
            uint64_t extent_size = 0; uint64_t seqno = 0; char id[33] = {0};
            size_t pvs_seen = 0, lvs_seen = 0;

            /* Reset the cursor just before the body and parse children */
            ps.pos = body_start;

            while (!p_eof(&ps) && depth > 0) {
                p_skip(&ps);
                if (p_eof(&ps)) break;
                if (ps.p[ps.pos] == '}') { ps.pos++; depth--; if (depth == 0) break; continue; }
                char child[64];
                size_t save = ps.pos;
                if (!p_ident(&ps, child, sizeof(child))) { ps.pos = save + 1; continue; }
                p_skip(&ps);
                if (ps.pos < ps.len && ps.p[ps.pos] == '=') {
                    ps.pos++;
                    p_skip(&ps);
                    if (wl_streq(child, "id"))            { char v[64]; if (p_string(&ps, v, sizeof(v))) { wl_strip_dashes(v); wl_strcpy(id, sizeof(id), v); has_id = 1; } }
                    else if (wl_streq(child, "system_id")) { p_string(&ps, vg->system_id, sizeof(vg->system_id)); }
                    else if (wl_streq(child, "seqno"))     { p_int(&ps, &seqno); }
                    else if (wl_streq(child, "extent_size")){ p_int(&ps, &extent_size); has_extent = 1; }
                    else {
                        /* skip a single value or array */
                        if (ps.pos < ps.len && ps.p[ps.pos] == '[') {
                            int d2 = 0;
                            while (ps.pos < ps.len) {
                                char c = ps.p[ps.pos];
                                if (c == '[') d2++; else if (c == ']') { d2--; if (d2 == 0) { ps.pos++; break; } }
                                ps.pos++;
                            }
                        } else {
                            while (ps.pos < ps.len && ps.p[ps.pos] != '\n') ps.pos++;
                        }
                    }
                } else if (ps.pos < ps.len && ps.p[ps.pos] == '{') {
                    /* a child sub-section */
                    if (wl_streq(child, "physical_volumes")) {
                        /* parse each pvN { } inside */
                        ps.pos++;  /* consume { */
                        for (;;) {
                            p_skip(&ps);
                            if (ps.pos >= ps.len || ps.p[ps.pos] == '}') { if (ps.pos < ps.len) ps.pos++; break; }
                            char pvkey[64];
                            if (!p_ident(&ps, pvkey, sizeof(pvkey))) { ps.pos++; continue; }
                            p_skip(&ps);
                            if (ps.pos < ps.len && ps.p[ps.pos] == '{') {
                                if (pvs_seen < (sizeof(vg->pvs) / sizeof(vg->pvs[0]))) {
                                    winlvm_physical_volume *pv = &vg->pvs[pvs_seen++];
                                    wl_zero(pv, sizeof(*pv));
                                    wl_strcpy(pv->key, sizeof(pv->key), pvkey);
                                    parse_pv_body(&ps, pv);
                                } else {
                                    vg->pv_overflow = 1;
                                    p_skip_section(&ps);
                                }
                            } else {
                                ps.pos++;
                            }
                        }
                    } else if (wl_streq(child, "logical_volumes")) {
                        ps.pos++;
                        for (;;) {
                            p_skip(&ps);
                            if (ps.pos >= ps.len || ps.p[ps.pos] == '}') { if (ps.pos < ps.len) ps.pos++; break; }
                            char lvname[64];
                            if (!p_ident(&ps, lvname, sizeof(lvname))) { ps.pos++; continue; }
                            p_skip(&ps);
                            if (ps.pos < ps.len && ps.p[ps.pos] == '{') {
                                if (lvs_seen < (sizeof(vg->lvs) / sizeof(vg->lvs[0]))) {
                                    winlvm_logical_volume *lv = &vg->lvs[lvs_seen++];
                                    wl_zero(lv, sizeof(*lv));
                                    wl_strcpy(lv->name, sizeof(lv->name), lvname);
                                    parse_lv_body(&ps, lv);
                                    /* mark mappable: visible + all segments striped + has segments */
                                    lv->mappable = 1;
                                    if (!lv->visible) { lv->mappable = 0; wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "hidden internal LV"); }
                                    if (lv->segment_count == 0) { lv->mappable = 0; wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "no segments"); }
                                    if (lv->segment_overflow) { lv->mappable = 0; wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "too many segments"); }
                                    for (size_t k = 0; k < lv->segment_count && lv->mappable; k++) {
                                        if (lv->segments[k].type != WINLVM_SEG_STRIPED) { lv->mappable = 0; wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "unsupported segment type"); }
                                        else if (lv->segments[k].stripe_overflow) { lv->mappable = 0; wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "too many stripes"); }
                                    }
                                } else {
                                    /* Record the overflow and still consume the
                                     * body so the parse position stays valid. */
                                    vg->lv_overflow = 1;
                                    p_skip_section(&ps);
                                }
                            } else {
                                ps.pos++;
                            }
                        }
                    } else {
                        /* skip arbitrary child section */
                        int d2 = 1; ps.pos++;
                        while (ps.pos < ps.len && d2 > 0) {
                            char c = ps.p[ps.pos];
                            if (c == '{') d2++; else if (c == '}') d2--;
                            ps.pos++;
                        }
                    }
                } else {
                    ps.pos = save + 1;
                }
            }

            if (has_id && has_extent) {
                /* Second pass, mirroring lvmfmt's collect_references: an LV
                 * named as some segment's 'origin' is a snapshot origin. Its
                 * extents describe the origin device, but the live mapping is
                 * a snapshot-origin target — mapping the extents directly
                 * bypasses copy-on-write and would destroy the snapshot. */
                for (size_t a = 0; a < lvs_seen; a++) {
                    if (!vg->lvs[a].mappable) continue;
                    for (size_t b = 0; b < lvs_seen; b++) {
                        for (size_t s = 0; s < vg->lvs[b].segment_count; s++) {
                            if (vg->lvs[b].segments[s].origin[0] &&
                                wl_streq(vg->lvs[b].segments[s].origin, vg->lvs[a].name)) {
                                vg->lvs[a].mappable = 0;
                                wl_strcpy(vg->lvs[a].unsupported_reason,
                                          sizeof(vg->lvs[a].unsupported_reason),
                                          "logical volume is the origin of a snapshot");
                            }
                        }
                    }
                }
                /* A dropped LV could have been a snapshot origin or an
                 * internal sub-volume; mapping anything from a truncated VG
                 * is unsafe. Refuse every LV, keep the reason on each. */
                if (vg->lv_overflow || vg->pv_overflow) {
                    for (size_t a = 0; a < lvs_seen; a++) {
                        if (vg->lvs[a].mappable) {
                            vg->lvs[a].mappable = 0;
                            wl_strcpy(vg->lvs[a].unsupported_reason,
                                      sizeof(vg->lvs[a].unsupported_reason),
                                      vg->lv_overflow ? "volume group has too many LVs" : "volume group has too many PVs");
                        }
                    }
                }
                wl_strcpy(vg->id, sizeof(vg->id), id);
                vg->seqno = seqno;
                vg->extent_size_sectors = extent_size;
                vg->pv_count = pvs_seen;
                vg->lv_count = lvs_seen;
                found_vg = 1;
                return 1;
            }
            /* Not the VG. Continue; we consumed its body above. Reset arrays. */
            pvs_seen = 0;
            lvs_seen = 0;
            wl_zero(vg, sizeof(*vg));
        } else {
            /* key = value at root (preamble). skip it. */
            if (ps.pos < ps.len && ps.p[ps.pos] == '=') {
                ps.pos++;
                if (ps.pos < ps.len && ps.p[ps.pos] == '[') {
                    int d = 0;
                    while (ps.pos < ps.len) {
                        char c = ps.p[ps.pos];
                        if (c == '[') d++; else if (c == ']') { d--; if (d == 0) { ps.pos++; break; } }
                        ps.pos++;
                    }
                } else {
                    while (ps.pos < ps.len && ps.p[ps.pos] != '\n') ps.pos++;
                }
            } else {
                ps.pos++;
            }
        }
    }
    (void)label;
    if (!found_vg) { return 0; }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public entry: winlvm_read_vg.                                       */
/* ------------------------------------------------------------------ */

static uint64_t peek_vg_seqno(const char *text, size_t text_len) {
    parser ps;
    p_init(&ps, text, text_len);
    while (!p_eof(&ps)) {
        p_skip(&ps);
        if (p_eof(&ps)) break;
        char ident[64];
        if (!p_ident(&ps, ident, sizeof(ident))) { ps.pos++; continue; }
        if (wl_streq(ident, "seqno")) {
            p_skip(&ps);
            if (ps.pos < ps.len && ps.p[ps.pos] == '=') {
                ps.pos++;
                uint64_t v = 0;
                if (p_int(&ps, &v)) return v;
            }
        }
    }
    return 0;
}

int winlvm_read_vg(winlvm_byte_source *src, winlvm_volume_group *vg,
                   winlvm_lvm_err *err) {
    if (!err) return 0;
    err->failed = 0;
    wl_zero(vg, sizeof(*vg));

    /* Step 1: label. */
    winlvm_pv_label label;
    wl_zero(&label, sizeof(label));
    if (!read_pv_label(src, &label, err)) return 0;

    /* Step 2/3: walk every metadata area, take the first readable one. */
    char *text = (char *)0;
    /* Kernel build: probe workers for multiple disks run concurrently, so
     * the scratch buffer must be per-call, not a shared static (the shared
     * static is a cross-thread corruption race — it caused the boot-time
     * SYSTEM_THREAD_EXCEPTION_NOT_HANDLED in winlvm.sys). User-mode tests
     * are single-threaded; the static keeps them allocation-free. */
#ifdef _KERNEL_MODE
    unsigned char *text_buf = (unsigned char *)0;
    {
        extern void *winlvm_probe_scratch_alloc(void);
        extern void winlvm_probe_scratch_free(void *p);
        text_buf = (unsigned char *)winlvm_probe_scratch_alloc();
        if (!text_buf) {
            wl_set_err(err, "out of memory reading LVM metadata");
            return 0;
        }
    }
#else
    static unsigned char text_buf[WINLVM_MAX_METADATA_BYTES + 1];
#endif
    text = (char *)text_buf;

    winlvm_lvm_err local_err;
    wl_zero(&local_err, sizeof(local_err));
    int got_text = 0;
    size_t text_len = 0;
    uint64_t best_seqno = 0;
    int result = 0;
    for (size_t i = 0; i < WINLVM_MAX_MD_AREAS && label.metadata_areas[i].size != 0; i++) {
        winlvm_mda_header mda;
        if (!read_mda_header(src, &label.metadata_areas[i], &mda, &local_err)) continue;
        if (read_metadata_text(src, &label.metadata_areas[i], &mda, text, &text_len, &local_err)) {
            uint64_t cand_seqno = peek_vg_seqno(text, text_len);
            if (!got_text || cand_seqno > best_seqno) {
                wl_zero(vg, sizeof(*vg));
                if (parse_vg(text, text_len, vg, &label, 0, 0)) {
                    best_seqno = cand_seqno;
                    got_text = 1;
                    result = 1;
                }
            }
        }
    }
    if (!result) {
        wl_set_err(err, local_err.failed ? local_err.message : "no readable metadata area");
    }
#ifdef _KERNEL_MODE
    winlvm_probe_scratch_free(text_buf);
#endif
    if (!result) return 0;

    if (vg->extent_size_sectors == 0) { wl_set_err(err, "VG declares a zero extent size"); return 0; }
    if (vg->pv_count == 0) { wl_set_err(err, "VG lists no physical volumes"); return 0; }

    /* Cross-check every metadata PV UUID has a matching label UUID. */
    int got_pv = 0;
    for (size_t i = 0; i < vg->pv_count; i++) {
        if (wl_streq(vg->pvs[i].id, label.pv_uuid)) { got_pv = 1; break; }
    }
    if (!got_pv) { wl_set_err(err, "metadata PV UUID does not match label UUID"); return 0; }

    return 1;
}
