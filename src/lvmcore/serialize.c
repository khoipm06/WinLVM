/*
 * serialize.c — freestanding C11 LVM2 metadata serializer & two-slot commit engine.
 *
 * Implements:
 *   1. Volume group → LVM2 text metadata serialization.
 *   2. Circular MDA ring buffer placement math (LVM2 _next_rlocn_offset analog).
 *   3. Two-slot commit engine: precommit writes slot 1, commit writes slot 0.
 *   4. Binary MDA header sector serialization with CRC32.
 *
 * Zero dynamic allocation, non-recursive, stack frames <= 512 bytes,
 * overflow-checked arithmetic throughout.
 */
#include "lvmcore/types.h"
#include "lvmcore/arena.h"
#include "lvmcore/crc32.h"
#include "lvmcore/parser.h"
#include "lvmcore/serialize.h"

#if defined(__GNUC__) || defined(__clang__)
#define LVMCORE_NOINLINE __attribute__((noinline))
#else
#define LVMCORE_NOINLINE
#endif

/* ------------------------------------------------------------------ */
/* Freestanding helpers                                                */
/* ------------------------------------------------------------------ */

static void s_memzero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    if (!b) return;
    while (n--) *b++ = 0;
}

static void s_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (!d || !s) return;
    while (n--) *d++ = *s++;
}

static size_t s_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

/* ------------------------------------------------------------------ */
/* Text formatting kernel (bounded, no printf)                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   truncated;
} text_writer;

static void tw_init(text_writer *tw, char *buf, size_t cap) {
    tw->buf = buf;
    tw->cap = cap;
    tw->len = 0;
    tw->truncated = false;
}

static void tw_putc(text_writer *tw, char c) {
    if (tw->len + 1 < tw->cap) {
        tw->buf[tw->len] = c;
    } else {
        tw->truncated = true;
    }
    tw->len++;
}

static void tw_puts(text_writer *tw, const char *s) {
    if (!s) return;
    while (*s) {
        tw_putc(tw, *s);
        s++;
    }
}

/* Append a decimal integer (unsigned 64-bit) without any library call. */
static void tw_put_u64(text_writer *tw, uint64_t v) {
    char tmp[24];
    size_t n = 0;
    if (v == 0) {
        tw_putc(tw, '0');
        return;
    }
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        tw_putc(tw, tmp[--n]);
    }
}

/* Append a string escaping backslash and double-quote characters for LVM2 config grammar. */
static void tw_puts_escaped(text_writer *tw, const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\\' || *s == '"') {
            tw_putc(tw, '\\');
        }
        tw_putc(tw, *s);
        s++;
    }
}

/* Append "key = value" line with newline. */
static void tw_line_str(text_writer *tw, const char *key, const char *val) {
    tw_puts(tw, key);
    tw_puts(tw, " = \"");
    tw_puts_escaped(tw, val);
    tw_puts(tw, "\"\n");
}

static void tw_line_u64(text_writer *tw, const char *key, uint64_t val) {
    tw_puts(tw, key);
    tw_puts(tw, " = ");
    tw_put_u64(tw, val);
    tw_putc(tw, '\n');
}

/* ------------------------------------------------------------------ */
/* UUID formatting: "6m0kE1-fm6x-OlV6-sDHg-xLe5-fAxG-mtK0Ow"           */
/* Parser strips dashes; we re-insert them in LVM2 groups of 6-4-4-4-4-4-6 */
/* ------------------------------------------------------------------ */

static void tw_put_uuid(text_writer *tw, const char *id_nodash) {
    /* id is 32 chars; insert dashes at positions 6,10,14,18,22,26 */
    static const int dash_pos[6] = {6, 10, 14, 18, 22, 26};
    size_t id_len = s_strlen(id_nodash);
    size_t di = 0;
    for (size_t i = 0; i < id_len; i++) {
        if (di < 6 && i == (size_t)dash_pos[di]) {
            tw_putc(tw, '-');
            di++;
        }
        tw_putc(tw, id_nodash[i]);
    }
}

/* ------------------------------------------------------------------ */
/* VG body serializers                                                */
/* ------------------------------------------------------------------ */

/* Full PV block writer — declared separately to keep frames small. */
LVMCORE_NOINLINE static void ser_pv_full(text_writer *tw,
                                         const lvmcore_physical_volume *pv,
                                         uint64_t pv_index) {
    tw_puts(tw, "\n\t\t");
    tw_puts(tw, pv->key);
    tw_puts(tw, " {\n");
    tw_puts(tw, "\t\t\tid = \"");
    tw_put_uuid(tw, pv->id);
    tw_puts(tw, "\"\n");
    if (pv->device[0]) {
        tw_puts(tw, "\t\t\tdevice = \"");
        tw_puts_escaped(tw, pv->device);
        tw_puts(tw, "\"\n");
    }
    if (pv->missing) {
        tw_puts(tw, "\n\t\t\tstatus = [\"ALLOCATABLE\", \"MISSING\"]\n");
    } else {
        tw_puts(tw, "\n\t\t\tstatus = [\"ALLOCATABLE\"]\n");
    }
    tw_puts(tw, "\t\t\tflags = []\n");
    tw_puts(tw, "\t\t\tdev_size = ");
    tw_put_u64(tw, pv->dev_size_sectors);
    tw_putc(tw, '\n');
    tw_puts(tw, "\t\t\tpe_start = ");
    tw_put_u64(tw, pv->pe_start);
    tw_putc(tw, '\n');
    tw_puts(tw, "\t\t\tpe_count = ");
    tw_put_u64(tw, pv->pe_count);
    tw_putc(tw, '\n');
    tw_puts(tw, "\t\t}\n");
    (void)pv_index;
}

LVMCORE_NOINLINE static void ser_segment_block(text_writer *tw,
                                               const lvmcore_segment *seg,
                                               uint64_t seg_index) {
    tw_puts(tw, "\n\t\t\tsegment");
    tw_put_u64(tw, seg_index);
    tw_puts(tw, " {\n");
    tw_puts(tw, "\t\t\t\tstart_extent = ");
    tw_put_u64(tw, seg->start_extent);
    tw_putc(tw, '\n');
    tw_puts(tw, "\t\t\t\textent_count = ");
    tw_put_u64(tw, seg->extent_count);
    tw_putc(tw, '\n');
    tw_puts(tw, "\n\t\t\t\ttype = \"");
    tw_puts(tw, seg->seg_type_name);
    tw_puts(tw, "\"\n");

    if (seg->type == LVMCORE_SEG_OTHER && seg->raw_body_len > 0 && seg->raw_body) {
        /* Unmapped segment types: re-emit verbatim keys captured at parse. */
        for (size_t i = 0; i < seg->raw_body_len; i++) {
            tw_putc(tw, seg->raw_body[i]);
        }
        if (seg->raw_body_len && seg->raw_body[seg->raw_body_len - 1] != '\n') {
            tw_putc(tw, '\n');
        }
        tw_puts(tw, "\t\t\t}\n");
        return;
    }

    tw_puts(tw, "\t\t\t\tstripe_count = ");
    tw_put_u64(tw, seg->stripe_count);
    tw_putc(tw, '\n');
    if (seg->stripe_count > 1) {
        tw_puts(tw, "\t\t\t\tstripe_size = ");
        tw_put_u64(tw, seg->stripe_size_sectors);
        tw_putc(tw, '\n');
    }
    tw_puts(tw, "\n\t\t\t\tstripes = [\n");
    for (size_t i = 0; i < seg->stripe_count_used; i++) {
        tw_puts(tw, "\t\t\t\t\t\"");
        tw_puts(tw, seg->stripes[i].pv_key);
        tw_puts(tw, "\", ");
        tw_put_u64(tw, seg->stripes[i].start_extent);
        if (i + 1 < seg->stripe_count_used) {
            tw_putc(tw, ',');
        }
        tw_putc(tw, '\n');
    }
    tw_puts(tw, "\t\t\t\t]\n");
    tw_puts(tw, "\t\t\t}\n");
}

LVMCORE_NOINLINE static void ser_lv_block(text_writer *tw,
                                          const lvmcore_logical_volume *lv) {
    tw_puts(tw, "\n\t\t");
    tw_puts(tw, lv->name);
    tw_puts(tw, " {\n");
    tw_puts(tw, "\t\t\tid = \"");
    tw_put_uuid(tw, lv->id);
    tw_puts(tw, "\"\n");
    if (lv->visible && !lv->read_only) {
        tw_puts(tw, "\t\t\tstatus = [\"READ\", \"WRITE\", \"VISIBLE\"]\n");
    } else if (lv->visible && lv->read_only) {
        tw_puts(tw, "\t\t\tstatus = [\"READ\", \"VISIBLE\"]\n");
    } else if (!lv->visible && !lv->read_only) {
        tw_puts(tw, "\t\t\tstatus = [\"READ\", \"WRITE\"]\n");
    } else {
        tw_puts(tw, "\t\t\tstatus = [\"READ\"]\n");
    }
    tw_puts(tw, "\t\t\tflags = []\n");
    tw_puts(tw, "\t\t\tcreation_time = 0\n");
    tw_puts(tw, "\t\t\tcreation_host = \"winlvm\"\n");
    tw_puts(tw, "\t\t\tsegment_count = ");
    tw_put_u64(tw, (uint64_t)lv->segment_count);
    tw_putc(tw, '\n');
    for (size_t i = 0; i < lv->segment_count; i++) {
        ser_segment_block(tw, &lv->segments[i], (uint64_t)(i + 1));
    }
    tw_puts(tw, "\t\t}\n");
}

LVMCORE_NOINLINE static void ser_vg_root(text_writer *tw,
                                         const lvmcore_volume_group *vg) {
    tw_puts(tw, "\n");
    tw_puts(tw, vg->name);
    tw_puts(tw, " {\n");
    tw_puts(tw, "\tid = \"");
    tw_put_uuid(tw, vg->id);
    tw_puts(tw, "\"\n");
    tw_line_u64(tw, "\tseqno", vg->seqno);
    tw_line_str(tw, "\tformat", "lvm2");
    if (vg->read_only) {
        tw_puts(tw, "\tstatus = [\"RESIZEABLE\", \"READ\"]\n");
    } else {
        tw_puts(tw, "\tstatus = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n");
    }
    tw_puts(tw, "\tflags = []\n");
    if (vg->system_id[0]) {
        tw_line_str(tw, "\tsystem_id", vg->system_id);
    }
    tw_line_u64(tw, "\textent_size", vg->extent_size_sectors);
    tw_line_u64(tw, "\tmax_lv", 0);
    tw_line_u64(tw, "\tmax_pv", 0);

    /* physical_volumes section */
    tw_puts(tw, "\n\tphysical_volumes {\n");
    for (size_t i = 0; i < vg->pv_count; i++) {
        ser_pv_full(tw, &vg->pvs[i], (uint64_t)i);
    }
    tw_puts(tw, "\t}\n");

    /* logical_volumes section */
    if (vg->lv_count > 0) {
        tw_puts(tw, "\n\tlogical_volumes {\n");
        for (size_t i = 0; i < vg->lv_count; i++) {
            ser_lv_block(tw, &vg->lvs[i]);
        }
        tw_puts(tw, "\t}\n");
    }

    tw_puts(tw, "}\n");
}

lvmcore_status lvmcore_serialize_vg(const lvmcore_volume_group *vg,
                                    char *out_buf,
                                    size_t out_cap,
                                    size_t *out_len) {
    if (!vg || !out_buf || !out_len) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (vg->name[0] == '\0' || vg->id[0] == '\0') {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (out_cap == 0) {
        return LVMCORE_ERR_OVERFLOW;
    }

    /* Validate arrays exist when counts are nonzero. */
    if ((vg->pv_count > 0 && !vg->pvs) || (vg->lv_count > 0 && !vg->lvs)) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    text_writer tw;
    tw_init(&tw, out_buf, out_cap);
    ser_vg_root(&tw, vg);

    if (tw.truncated) {
        return LVMCORE_ERR_OVERFLOW;
    }

    out_buf[tw.len] = '\0';
    /* LVM2 rlocn.size convention: size includes trailing NUL. */
    *out_len = tw.len + 1;
    return LVMCORE_OK;
}

/* ------------------------------------------------------------------ */
/* MDA ring buffer placement (_next_rlocn_offset analog)              */
/* ------------------------------------------------------------------ */

lvmcore_status lvmcore_mda_next_offset(const lvmcore_mda_header *mdah,
                                       uint64_t mda_size,
                                       uint64_t payload_size,
                                       uint64_t alignment,
                                       uint64_t *next_offset_out) {
    if (!next_offset_out) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    *next_offset_out = 0;

    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (mda_size < LVMCORE_MDA_HEADER_SIZE) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    uint64_t ring_size = mda_size - LVMCORE_MDA_HEADER_SIZE;
    uint64_t ring_usable = ring_size / 2;
    if (ring_usable >= LVMCORE_SECTOR_BYTES) {
        ring_usable -= LVMCORE_SECTOR_BYTES;
    } else {
        return LVMCORE_ERR_OVERFLOW;
    }

    if (payload_size > ring_usable) {
        return LVMCORE_ERR_OVERFLOW;
    }

    /* No committed metadata → begin at header boundary. */
    if (!mdah) {
        *next_offset_out = LVMCORE_MDA_HEADER_SIZE;
        return LVMCORE_OK;
    }

    const lvmcore_raw_locn *old = &mdah->raw_locns[0];
    if ((old->flags & LVMCORE_RAW_LOCN_IGNORED) != 0 ||
        (old->offset == 0 && old->size == 0)) {
        *next_offset_out = LVMCORE_MDA_HEADER_SIZE;
        return LVMCORE_OK;
    }

    if (old->offset < LVMCORE_MDA_HEADER_SIZE || old->offset >= mda_size ||
        old->size == 0 || old->size > ring_size) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    /* Compute old_last: last byte of old metadata (inclusive). */
    uint64_t old_last;
    if (old->size > mda_size - old->offset) {
        uint64_t old_wrap = old->size - (mda_size - old->offset);
        old_last = old_wrap + LVMCORE_MDA_HEADER_SIZE - 1;
    } else {
        old_last = old->offset + old->size - 1;
    }

    if (old_last >= mda_size) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    /* If less than alignment bytes remain before end, wrap to beginning. */
    if (mda_size - old_last < alignment) {
        *next_offset_out = LVMCORE_MDA_HEADER_SIZE;
        return LVMCORE_OK;
    }

    /* New metadata begins after the old, rounded up to alignment. */
    uint64_t next_start = old_last + 1;
    uint64_t new_start = next_start;
    if (next_start % alignment != 0) {
        uint64_t adjust = alignment - (next_start % alignment);
        if (adjust > UINT64_MAX - next_start) {
            return LVMCORE_ERR_OVERFLOW;
        }
        new_start = next_start + adjust;
    }

    /* If beyond end (or within alignment of end), wrap to beginning. */
    if (new_start > mda_size - alignment) {
        *next_offset_out = LVMCORE_MDA_HEADER_SIZE;
        return LVMCORE_OK;
    }

    *next_offset_out = new_start;
    return LVMCORE_OK;
}

/* ------------------------------------------------------------------ */
/* Two-slot commit engine                                              */
/* ------------------------------------------------------------------ */

lvmcore_status lvmcore_mda_prepare_precommit(const lvmcore_mda_header *cur_mdah,
                                             uint64_t new_offset,
                                             uint64_t new_size,
                                             uint32_t new_crc,
                                             lvmcore_mda_header *new_mdah) {
    if (!new_mdah) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (new_size > 0 &&
        (new_offset < LVMCORE_MDA_HEADER_SIZE)) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    lvmcore_mda_header tmp;
    if (cur_mdah) {
        tmp = *cur_mdah;
    } else {
        s_memzero(&tmp, sizeof(tmp));
        tmp.version = 1;
    }

    /* Slot 0 preserved; slot 1 = new location. */
    tmp.raw_locns[1].offset   = new_offset;
    tmp.raw_locns[1].size     = new_size;
    tmp.raw_locns[1].checksum = new_crc;
    tmp.raw_locns[1].flags    = 0;

    *new_mdah = tmp;
    return LVMCORE_OK;
}

lvmcore_status lvmcore_mda_prepare_commit(const lvmcore_mda_header *cur_mdah,
                                          uint64_t new_offset,
                                          uint64_t new_size,
                                          uint32_t new_crc,
                                          lvmcore_mda_header *new_mdah) {
    if (!new_mdah) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (new_size > 0 && new_offset < LVMCORE_MDA_HEADER_SIZE) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    lvmcore_mda_header tmp;
    if (cur_mdah) {
        tmp = *cur_mdah;
    } else {
        s_memzero(&tmp, sizeof(tmp));
        tmp.version = 1;
    }

    /* Slot 0 = new location; slot 1 cleared. */
    tmp.raw_locns[0].offset   = new_offset;
    tmp.raw_locns[0].size     = new_size;
    tmp.raw_locns[0].checksum = new_crc;
    tmp.raw_locns[0].flags    = 0;

    tmp.raw_locns[1].offset   = 0;
    tmp.raw_locns[1].size     = 0;
    tmp.raw_locns[1].checksum = 0;
    tmp.raw_locns[1].flags    = 0;

    *new_mdah = tmp;
    return LVMCORE_OK;
}

/* ------------------------------------------------------------------ */
/* Binary MDA header sector serialization                             */
/* ------------------------------------------------------------------ */

static const uint8_t MDA_MAGIC[16] = {
    ' ', 'L', 'V', 'M', '2', ' ', 'x', '[', '5', 'A', '%', 'r', '0', 'N', '*', '>'
};

static void s_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void s_put_le64(uint8_t *p, uint64_t v) {
    s_put_le32(p, (uint32_t)(v & 0xffffffffu));
    s_put_le32(p + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

lvmcore_status lvmcore_mda_header_write(const lvmcore_mda_header *mdah,
                                        uint8_t *sector_buf,
                                        size_t sector_cap) {
    if (!mdah || !sector_buf) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (sector_cap < LVMCORE_MDA_HEADER_SIZE) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    s_memzero(sector_buf, LVMCORE_MDA_HEADER_SIZE);

    /* checksum placeholder at 0..3; magic at 4..19; version at 20..23 */
    s_memcpy(sector_buf + 4, MDA_MAGIC, sizeof(MDA_MAGIC));
    s_put_le32(sector_buf + 20, mdah->version != 0 ? mdah->version : 1);
    s_put_le64(sector_buf + 24, mdah->start);
    s_put_le64(sector_buf + 32, mdah->size);

    /*
     * Packed 24-byte raw_locn slots follow the 40-byte mda_header prefix:
     *   Slot 0 (committed):    bytes 40..63
     *   Slot 1 (precommitted): bytes 64..87
     * Remaining bytes 88..511 stay zero (NULL-terminating raw_locns[]).
     */
    for (size_t i = 0; i < 2; i++) {
        size_t base = 40u + i * 24u;
        s_put_le64(sector_buf + base,      mdah->raw_locns[i].offset);
        s_put_le64(sector_buf + base + 8,  mdah->raw_locns[i].size);
        s_put_le32(sector_buf + base + 16, mdah->raw_locns[i].checksum);
        s_put_le32(sector_buf + base + 20, mdah->raw_locns[i].flags);
    }

    uint32_t crc = lvmcore_crc32(sector_buf + 4,
                                 LVMCORE_MDA_HEADER_SIZE - 4);
    s_put_le32(sector_buf, crc);

    return LVMCORE_OK;
}
