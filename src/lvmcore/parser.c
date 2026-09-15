/*
 * parser.c — freestanding C11 LVM2 metadata parser.
 *
 * Implements non-recursive, bounds-checked parsing with zero dynamic
 * allocations. All state lives in caller-supplied scratch arena.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lvmcore/types.h"
#include "lvmcore/arena.h"
#include "lvmcore/crc32.h"
#include "lvmcore/parser.h"

#if defined(__GNUC__) || defined(__clang__)
#define LVMCORE_NOINLINE __attribute__((noinline))
#else
#define LVMCORE_NOINLINE
#endif

/* ------------------------------------------------------------------ */
/* Freestanding string and memory helpers                              */
/* ------------------------------------------------------------------ */

static void wl_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static void wl_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void wl_memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
}

static int wl_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p1 = (const uint8_t *)a;
    const uint8_t *p2 = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return p1[i] < p2[i] ? -1 : 1;
    }
    return 0;
}

static size_t wl_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static int wl_streq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void wl_strcpy(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < dst_cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void wl_strip_dashes(char *s) {
    if (!s) return;
    char *d = s;
    while (*s) {
        if (*s != '-') *d++ = *s;
        s++;
    }
    *d = '\0';
}

static void wl_set_err(lvmcore_err *err, lvmcore_status status, const char *msg) {
    if (!err) return;
    err->status = status;
    err->failed = (status != LVMCORE_OK);
    wl_strcpy(err->message, sizeof(err->message), msg ? msg : "");
}

const char *lvmcore_status_string(lvmcore_status status) {
    switch (status) {
    case LVMCORE_OK: return "OK";
    case LVMCORE_ERR_INVALID_ARG: return "invalid argument";
    case LVMCORE_ERR_NO_LABEL: return "no PV label found";
    case LVMCORE_ERR_CORRUPT_LABEL: return "corrupted PV label";
    case LVMCORE_ERR_CORRUPT_MDA: return "corrupted metadata area header";
    case LVMCORE_ERR_NO_VALID_MDA: return "no valid metadata area found";
    case LVMCORE_ERR_CORRUPT_METADATA: return "corrupted metadata text";
    case LVMCORE_ERR_NO_VG: return "no volume group found in metadata";
    case LVMCORE_ERR_SCRATCH_EXHAUSTED: return "scratch arena exhausted";
    case LVMCORE_ERR_UNSUPPORTED: return "unsupported LVM format or configuration";
    case LVMCORE_ERR_IO: return "I/O error reading device";
    case LVMCORE_ERR_OVERFLOW: return "integer overflow";
    default: return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* Little-endian decoders                                              */
/* ------------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* ------------------------------------------------------------------ */
/* Bounds-checked source read                                         */
/* ------------------------------------------------------------------ */

static bool src_read_full(lvmcore_byte_source *src, uint64_t off, void *buf, size_t len) {
    if (!src || !src->read || !buf) return false;
    if (off > src->size || (uint64_t)len > src->size - off) return false;
    size_t got = src->read(src->impl, off, buf, len);
    return got == len;
}

/* ------------------------------------------------------------------ */
/* Arena-based vector helper                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    void   *data;
    size_t  count;
    size_t  capacity;
    size_t  elem_size;
} arena_vec;

static bool arena_vec_init(arena_vec *v, lvmcore_arena *arena, size_t initial_cap, size_t elem_size) {
    v->count = 0;
    v->capacity = initial_cap ? initial_cap : 4;
    v->elem_size = elem_size;
    v->data = lvmcore_arena_alloc_zero(arena, v->capacity * elem_size, 8);
    return v->data != NULL;
}

static bool arena_vec_push(arena_vec *v, lvmcore_arena *arena, const void *elem) {
    if (v->count >= v->capacity) {
        size_t new_cap = v->capacity * 2;
        if (new_cap < 8) new_cap = 8;
        void *new_data = lvmcore_arena_alloc_zero(arena, new_cap * v->elem_size, 8);
        if (!new_data) return false;
        wl_memcpy(new_data, v->data, v->count * v->elem_size);
        v->data = new_data;
        v->capacity = new_cap;
    }
    wl_memcpy((uint8_t *)v->data + (v->count * v->elem_size), elem, v->elem_size);
    v->count++;
    return true;
}

static void *arena_vec_push_empty(arena_vec *v, lvmcore_arena *arena) {
    if (v->count >= v->capacity) {
        size_t new_cap = v->capacity * 2;
        if (new_cap < 8) new_cap = 8;
        void *new_data = lvmcore_arena_alloc_zero(arena, new_cap * v->elem_size, 8);
        if (!new_data) return NULL;
        wl_memcpy(new_data, v->data, v->count * v->elem_size);
        v->data = new_data;
        v->capacity = new_cap;
    }
    void *elem = (uint8_t *)v->data + (v->count * v->elem_size);
    wl_zero(elem, v->elem_size);
    v->count++;
    return elem;
}

/* ------------------------------------------------------------------ */
/* 1. Label Scanner & PV Header Parse                                 */
/* ------------------------------------------------------------------ */

static const char LABEL_MAGIC[8] = { 'L','A','B','E','L','O','N','E' };
static const char LABEL_TYPE[8]  = { 'L','V','M','2',' ','0','0','1' };

static bool parse_area_list(const uint8_t *buf, size_t bufsz, size_t *cursor,
                            arena_vec *out_vec, lvmcore_arena *arena,
                            lvmcore_err *err) {
    for (;;) {
        if (*cursor + 16 > bufsz) {
            wl_set_err(err, LVMCORE_ERR_CORRUPT_LABEL, "PV header truncated in disk-area list");
            return false;
        }
        uint64_t off = le64(buf + *cursor);
        uint64_t sz  = le64(buf + *cursor + 8);
        *cursor += 16;
        if (off == 0 && sz == 0) {
            /* Sentinel reached */
            return true;
        }
        lvmcore_disk_area da;
        da.offset = off;
        da.size = sz;
        if (!arena_vec_push(out_vec, arena, &da)) {
            wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted parsing disk areas");
            return false;
        }
    }
}

lvmcore_status lvmcore_read_pv_label(lvmcore_byte_source *src,
                                     lvmcore_arena *arena,
                                     lvmcore_pv_label *label_out,
                                     lvmcore_err *err) {
    if (!src || !arena || !label_out) {
        wl_set_err(err, LVMCORE_ERR_INVALID_ARG, "invalid arguments to lvmcore_read_pv_label");
        return LVMCORE_ERR_INVALID_ARG;
    }
    wl_zero(label_out, sizeof(*label_out));

    /* Allocate scan buffer in arena to guarantee stack frame budget <= 512 bytes */
    uint8_t *head = (uint8_t *)lvmcore_arena_alloc(arena, LVMCORE_LABEL_SCAN_BYTES, 8);
    if (!head) {
        wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted for label scan buffer");
        return LVMCORE_ERR_SCRATCH_EXHAUSTED;
    }

    if (!src_read_full(src, 0, head, LVMCORE_LABEL_SCAN_BYTES)) {
        wl_set_err(err, LVMCORE_ERR_IO, "failed reading device head for label scan");
        return LVMCORE_ERR_IO;
    }

    for (size_t off = 0; off + LVMCORE_LABEL_SIZE <= LVMCORE_LABEL_SCAN_BYTES; off += LVMCORE_LABEL_SIZE) {
        const uint8_t *p = head + off;
        if (wl_memcmp(p, LABEL_MAGIC, sizeof(LABEL_MAGIC)) != 0) continue;

        uint32_t stored_crc = le32(p + 16);
        uint32_t computed_crc = lvmcore_crc32(p + 20, LVMCORE_LABEL_SIZE - 20);
        if (stored_crc != computed_crc) {
            wl_set_err(err, LVMCORE_ERR_CORRUPT_LABEL, "PV label CRC mismatch");
            return LVMCORE_ERR_CORRUPT_LABEL;
        }

        if (wl_memcmp(p + 24, LABEL_TYPE, sizeof(LABEL_TYPE)) != 0) {
            wl_set_err(err, LVMCORE_ERR_UNSUPPORTED, "unsupported PV label type");
            return LVMCORE_ERR_UNSUPPORTED;
        }

        uint32_t data_off = le32(p + 20);
        if (data_off < 32 || data_off >= LVMCORE_LABEL_SIZE) {
            wl_set_err(err, LVMCORE_ERR_CORRUPT_LABEL, "PV label data offset out of range");
            return LVMCORE_ERR_CORRUPT_LABEL;
        }

        label_out->label_offset = off;
        label_out->label_sector = le64(p + 8);

        const uint8_t *pvh = p + data_off;
        wl_memcpy(label_out->pv_uuid, pvh, 32);
        label_out->pv_uuid[32] = '\0';
        label_out->device_size = le64(pvh + 32);

        arena_vec data_areas_vec;
        arena_vec meta_areas_vec;
        if (!arena_vec_init(&data_areas_vec, arena, 4, sizeof(lvmcore_disk_area)) ||
            !arena_vec_init(&meta_areas_vec, arena, 4, sizeof(lvmcore_disk_area))) {
            wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted initializing disk area arrays");
            return LVMCORE_ERR_SCRATCH_EXHAUSTED;
        }

        size_t cursor = 40;
        size_t pvh_max = LVMCORE_LABEL_SIZE - data_off;
        if (!parse_area_list(pvh, pvh_max, &cursor, &data_areas_vec, arena, err)) {
            return err ? err->status : LVMCORE_ERR_CORRUPT_LABEL;
        }
        if (!parse_area_list(pvh, pvh_max, &cursor, &meta_areas_vec, arena, err)) {
            return err ? err->status : LVMCORE_ERR_CORRUPT_LABEL;
        }

        label_out->data_area_count = data_areas_vec.count;
        label_out->data_areas = (lvmcore_disk_area *)data_areas_vec.data;
        label_out->metadata_area_count = meta_areas_vec.count;
        label_out->metadata_areas = (lvmcore_disk_area *)meta_areas_vec.data;

        wl_set_err(err, LVMCORE_OK, NULL);
        return LVMCORE_OK;
    }

    wl_set_err(err, LVMCORE_ERR_NO_LABEL, "no LVM2 PV label found in first 8 KiB");
    return LVMCORE_ERR_NO_LABEL;
}

/* ------------------------------------------------------------------ */
/* 2. MDA Header Parser                                                */
/* ------------------------------------------------------------------ */

static const char MDA_MAGIC[16] = {
    ' ','L','V','M','2',' ','x','[','5','A','%','r','0','N','*','>'
};

lvmcore_status lvmcore_read_mda_header(lvmcore_byte_source *src,
                                       const lvmcore_disk_area *area,
                                       lvmcore_arena *arena,
                                       lvmcore_mda_header *mda_out,
                                       lvmcore_err *err) {
    if (!src || !area || !arena || !mda_out) {
        wl_set_err(err, LVMCORE_ERR_INVALID_ARG, "invalid arguments to lvmcore_read_mda_header");
        return LVMCORE_ERR_INVALID_ARG;
    }
    wl_zero(mda_out, sizeof(*mda_out));

    if (area->size < LVMCORE_MDA_HEADER_SIZE ||
        area->offset > src->size ||
        area->size > src->size - area->offset) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "metadata area lies outside device boundaries");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    /* Allocate buffer from arena to keep stack frame budget well under 512 bytes */
    uint8_t *buf = (uint8_t *)lvmcore_arena_alloc(arena, LVMCORE_MDA_HEADER_SIZE, 8);
    if (!buf) {
        wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted for mda_header buffer");
        return LVMCORE_ERR_SCRATCH_EXHAUSTED;
    }

    if (!src_read_full(src, area->offset, buf, LVMCORE_MDA_HEADER_SIZE)) {
        wl_set_err(err, LVMCORE_ERR_IO, "failed reading mda_header");
        return LVMCORE_ERR_IO;
    }

    uint32_t stored_crc = le32(buf);
    uint32_t computed_crc = lvmcore_crc32(buf + 4, LVMCORE_MDA_HEADER_SIZE - 4);
    if (stored_crc != computed_crc) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "mda_header CRC mismatch");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    if (wl_memcmp(buf + 4, MDA_MAGIC, sizeof(MDA_MAGIC)) != 0) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "mda_header magic mismatch");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    uint32_t version = le32(buf + 20);
    if (version != 1) {
        wl_set_err(err, LVMCORE_ERR_UNSUPPORTED, "unsupported mda_header version");
        return LVMCORE_ERR_UNSUPPORTED;
    }

    mda_out->start = le64(buf + 24);
    mda_out->size = le64(buf + 32);
    mda_out->version = version;

    if (mda_out->start != area->offset || mda_out->size != area->size) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "mda_header start/size disagrees with PV header");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    /* Slot 0 (committed) */
    mda_out->raw_locns[0].offset   = le64(buf + 40);
    mda_out->raw_locns[0].size     = le64(buf + 48);
    mda_out->raw_locns[0].checksum = le32(buf + 56);
    mda_out->raw_locns[0].flags    = le32(buf + 60);

    /* Slot 1 (precommitted) */
    mda_out->raw_locns[1].offset   = le64(buf + 64);
    mda_out->raw_locns[1].size     = le64(buf + 72);
    mda_out->raw_locns[1].checksum = le32(buf + 80);
    mda_out->raw_locns[1].flags    = le32(buf + 88);

    wl_set_err(err, LVMCORE_OK, NULL);
    return LVMCORE_OK;
}

/* ------------------------------------------------------------------ */
/* 3. Ring-Buffer Metadata Text Extractor                              */
/* ------------------------------------------------------------------ */

lvmcore_status lvmcore_read_metadata_text(lvmcore_byte_source *src,
                                          const lvmcore_disk_area *area,
                                          const lvmcore_mda_header *mda,
                                          uint32_t flags,
                                          lvmcore_arena *arena,
                                          char **text_out,
                                          size_t *text_len_out,
                                          bool *is_precommitted_out,
                                          lvmcore_err *err) {
    if (!src || !area || !mda || !arena || !text_out || !text_len_out) {
        wl_set_err(err, LVMCORE_ERR_INVALID_ARG, "invalid arguments to lvmcore_read_metadata_text");
        return LVMCORE_ERR_INVALID_ARG;
    }
    *text_out = NULL;
    *text_len_out = 0;
    if (is_precommitted_out) *is_precommitted_out = false;

    const lvmcore_raw_locn *rlocn = &mda->raw_locns[0];
    bool use_precommitted = false;

    if ((flags & LVMCORE_READ_FLAG_PRECOMMITTED) &&
        (mda->raw_locns[1].flags & LVMCORE_RAW_LOCN_IGNORED) == 0 &&
        mda->raw_locns[1].size > 0 &&
        mda->raw_locns[1].offset != mda->raw_locns[0].offset) {
        rlocn = &mda->raw_locns[1];
        use_precommitted = true;
    }

    if (rlocn->size == 0) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "metadata area contains no committed metadata");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    if (rlocn->size > LVMCORE_MAX_METADATA_BYTES) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "committed metadata descriptor is out of range");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    if (rlocn->offset < LVMCORE_MDA_HEADER_SIZE || rlocn->offset >= area->size) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "committed metadata offset inside header or past area");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    uint64_t ring_begin = area->offset + LVMCORE_MDA_HEADER_SIZE;
    uint64_t ring_size = area->size - LVMCORE_MDA_HEADER_SIZE;
    if (rlocn->size > ring_size) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "metadata larger than the metadata area");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    uint64_t rel = rlocn->offset - LVMCORE_MDA_HEADER_SIZE;
    uint64_t first = (rlocn->size < ring_size - rel) ? rlocn->size : (ring_size - rel);
    uint64_t wrapped = rlocn->size - first;

    char *buf = (char *)lvmcore_arena_alloc(arena, (size_t)rlocn->size + 1, 1);
    if (!buf) {
        wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted for metadata text buffer");
        return LVMCORE_ERR_SCRATCH_EXHAUSTED;
    }

    uint64_t abs_commit_offset = area->offset + rlocn->offset;
    if (!src_read_full(src, abs_commit_offset, buf, (size_t)first)) {
        wl_set_err(err, LVMCORE_ERR_IO, "failed reading metadata text");
        return LVMCORE_ERR_IO;
    }

    if (wrapped > 0) {
        if (!src_read_full(src, ring_begin, buf + first, (size_t)wrapped)) {
            wl_set_err(err, LVMCORE_ERR_IO, "failed reading wrapped metadata text");
            return LVMCORE_ERR_IO;
        }
    }

    uint32_t computed_crc = lvmcore_crc32(buf, (size_t)rlocn->size);
    if (computed_crc != rlocn->checksum) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_METADATA, "metadata text CRC mismatch");
        return LVMCORE_ERR_CORRUPT_METADATA;
    }

    size_t len = (size_t)rlocn->size;
    buf[len] = '\0';
    while (len > 0 && buf[len - 1] == '\0') {
        len--;
    }

    *text_out = buf;
    *text_len_out = len;
    if (is_precommitted_out) *is_precommitted_out = use_precommitted;

    wl_set_err(err, LVMCORE_OK, NULL);
    return LVMCORE_OK;
}

/* ------------------------------------------------------------------ */
/* 4. Text Metadata Grammar Parser                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    size_t len;
    size_t pos;
} parser;

static void p_init(parser *ps, const char *s, size_t n) {
    ps->p = s;
    ps->len = n;
    ps->pos = 0;
}

static bool p_eof(const parser *ps) {
    return ps->pos >= ps->len;
}

static bool is_ident_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           c == '_' || c == '-' || c == '.' || c == '+' || c == '/';
}

static void p_skip(parser *ps) {
    while (ps->pos < ps->len) {
        char c = ps->p[ps->pos];
        if (c == '#') {
            while (ps->pos < ps->len && ps->p[ps->pos] != '\n') ps->pos++;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0') {
            ps->pos++;
        } else {
            break;
        }
    }
}

static bool p_ident(parser *ps, char *out, size_t cap) {
    p_skip(ps);
    size_t start = ps->pos;
    while (ps->pos < ps->len && is_ident_char(ps->p[ps->pos])) ps->pos++;
    if (ps->pos == start) return false;
    size_t n = ps->pos - start;
    if (n >= cap) n = cap - 1;
    wl_memmove(out, ps->p + start, n);
    out[n] = '\0';
    return true;
}

static bool p_string(parser *ps, char *out, size_t cap) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '"') return false;
    ps->pos++;
    size_t i = 0;
    while (ps->pos < ps->len && ps->p[ps->pos] != '"') {
        if (ps->p[ps->pos] == '\\' && ps->pos + 1 < ps->len) {
            ps->pos++; /* skip escape */
        }
        if (i + 1 < cap) {
            out[i++] = ps->p[ps->pos];
        }
        if (ps->pos < ps->len) ps->pos++;
    }
    if (ps->pos >= ps->len) return false;
    ps->pos++; /* closing quote */
    out[i] = '\0';
    return true;
}

static bool p_int(parser *ps, uint64_t *out) {
    p_skip(ps);
    bool neg = false;
    if (ps->pos < ps->len && (ps->p[ps->pos] == '-' || ps->p[ps->pos] == '+')) {
        neg = (ps->p[ps->pos] == '-');
        ps->pos++;
    }
    if (ps->pos >= ps->len || ps->p[ps->pos] < '0' || ps->p[ps->pos] > '9') return false;
    uint64_t v = 0;
    const uint64_t max_val = (uint64_t)0x7fffffffffffffffull; /* INT64_MAX */
    while (ps->pos < ps->len && ps->p[ps->pos] >= '0' && ps->p[ps->pos] <= '9') {
        uint64_t digit = (uint64_t)(ps->p[ps->pos] - '0');
        if (v > (max_val - digit) / 10) {
            v = max_val;
            while (ps->pos < ps->len && ps->p[ps->pos] >= '0' && ps->p[ps->pos] <= '9') ps->pos++;
            break;
        } else {
            v = v * 10 + digit;
        }
        ps->pos++;
    }
    *out = neg ? 0 : v;
    return true;
}

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

static void p_skip_value(parser *ps) {
    p_skip(ps);
    if (ps->pos < ps->len && ps->p[ps->pos] == '[') {
        int depth = 0;
        while (ps->pos < ps->len) {
            char c = ps->p[ps->pos];
            if (c == '[') depth++;
            else if (c == ']') {
                depth--;
                if (depth == 0) {
                    ps->pos++;
                    break;
                }
            } else if (c == '{') {
                p_skip_section(ps);
            }
            ps->pos++;
        }
    } else {
        while (ps->pos < ps->len && ps->p[ps->pos] != '\n') ps->pos++;
    }
}

LVMCORE_NOINLINE static void parse_stripes(parser *ps, lvmcore_segment *seg, lvmcore_arena *arena) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '[') return;
    ps->pos++;

    arena_vec stripes_vec;
    if (!arena_vec_init(&stripes_vec, arena, 4, sizeof(lvmcore_stripe))) {
        seg->stripe_overflow = 1;
        p_skip_value(ps);
        return;
    }

    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len) break;
        char c = ps->p[ps->pos];
        if (c == ']') {
            ps->pos++;
            break;
        }
        if (c == ',') {
            ps->pos++;
            continue;
        }
        if (c == '"') {
            char target[64];
            if (!p_string(ps, target, sizeof(target))) break;
            p_skip(ps);
            if (ps->pos < ps->len && ps->p[ps->pos] == ',') ps->pos++;
            uint64_t start_ext = 0;
            p_int(ps, &start_ext);

            lvmcore_stripe st;
            wl_strcpy(st.pv_key, sizeof(st.pv_key), target);
            st.start_extent = start_ext;
            if (!arena_vec_push(&stripes_vec, arena, &st)) {
                seg->stripe_overflow = 1;
            }
        } else {
            ps->pos++;
        }
    }

    seg->stripes = (lvmcore_stripe *)stripes_vec.data;
    seg->stripe_count_used = stripes_vec.count;
}

static void seg_append_raw(parser *ps, lvmcore_segment *seg, lvmcore_arena *arena,
                           size_t key_start, size_t val_start) {
    size_t val_end = ps->pos;
    while (val_end > val_start && (ps->p[val_end - 1] == '\n' || ps->p[val_end - 1] == '\r')) val_end--;
    if (val_end <= val_start) return;
    size_t span = val_end - key_start;
    char *nb = (char *)lvmcore_arena_alloc(arena, seg->raw_body_len + span + 2, 1);
    if (!nb) return;
    if (seg->raw_body_len) {
        wl_memcpy(nb, seg->raw_body, seg->raw_body_len);
        nb[seg->raw_body_len++] = '\n';
    }
    wl_memcpy(nb + seg->raw_body_len, ps->p + key_start, span);
    seg->raw_body_len += span;
    nb[seg->raw_body_len] = '\0';
    seg->raw_body = nb;
}

LVMCORE_NOINLINE static void parse_segment(parser *ps, lvmcore_segment *seg, lvmcore_arena *arena) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '{') return;
    ps->pos++;

    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') {
            if (ps->pos < ps->len) ps->pos++;
            return;
        }
        char key[64];
        size_t key_start = ps->pos;
        if (!p_ident(ps, key, sizeof(key))) {
            ps->pos++;
            continue;
        }
        p_skip(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == '=') {
            ps->pos++;
            p_skip(ps);
            if (wl_streq(key, "start_extent")) {
                p_int(ps, &seg->start_extent);
            } else if (wl_streq(key, "extent_count")) {
                p_int(ps, &seg->extent_count);
            } else if (wl_streq(key, "stripe_count")) {
                p_int(ps, &seg->stripe_count);
            } else if (wl_streq(key, "stripe_size")) {
                p_int(ps, &seg->stripe_size_sectors);
            } else if (wl_streq(key, "origin")) {
                size_t val_start = ps->pos;
                p_string(ps, seg->origin, sizeof(seg->origin));
                /* Also raw-capture: SEG_OTHER (snapshot) serializer re-emits origin from raw body. */
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "type")) {
                char t[32];
                if (p_string(ps, t, sizeof(t)) || p_ident(ps, t, sizeof(t))) {
                    wl_strcpy(seg->seg_type_name, sizeof(seg->seg_type_name), t);
                    if (wl_streq(t, "striped") || wl_streq(t, "linear")) {
                        seg->type = LVMCORE_SEG_STRIPED;
                    } else {
                        seg->type = LVMCORE_SEG_OTHER;
                    }
                }
            } else if (wl_streq(key, "stripes")) {
                parse_stripes(ps, seg, arena);
            } else {
                /* Unmapped key: capture raw "key = value" span for SEG_OTHER re-emit. */
                size_t val_start = ps->pos;
                p_skip_value(ps);
                seg_append_raw(ps, seg, arena, key_start, val_start);
            }
        } else if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            p_skip_section(ps);
        } else {
            ps->pos++;
        }
    }
}

LVMCORE_NOINLINE static void parse_lv_body(parser *ps, lvmcore_logical_volume *lv, lvmcore_arena *arena) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '{') return;
    ps->pos++;

    arena_vec seg_vec;
    if (!arena_vec_init(&seg_vec, arena, 4, sizeof(lvmcore_segment))) {
        lv->segment_overflow = 1;
    }

    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') {
            if (ps->pos < ps->len) ps->pos++;
            break;
        }
        char key[64];
        if (!p_ident(ps, key, sizeof(key))) {
            ps->pos++;
            continue;
        }
        p_skip(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == '=') {
            ps->pos++;
            p_skip(ps);
            if (wl_streq(key, "segment_count")) {
                uint64_t v = 0;
                p_int(ps, &v);
                lv->declared_segment_count = v;
            } else if (wl_streq(key, "id")) {
                char v[64];
                if (p_string(ps, v, sizeof(v))) {
                    wl_strip_dashes(v);
                    wl_strcpy(lv->id, sizeof(lv->id), v);
                }
            } else if (wl_streq(key, "status")) {
                if (ps->pos < ps->len && ps->p[ps->pos] == '[') {
                    ps->pos++;
                    for (;;) {
                        p_skip(ps);
                        if (ps->pos >= ps->len || ps->p[ps->pos] == ']') {
                            if (ps->pos < ps->len) ps->pos++;
                            break;
                        }
                        if (ps->p[ps->pos] == ',') {
                            ps->pos++;
                            continue;
                        }
                        char s[32];
                        if (p_string(ps, s, sizeof(s))) {
                            if (wl_streq(s, "VISIBLE")) lv->visible = 1;
                        } else {
                            ps->pos++;
                        }
                    }
                }
            } else {
                p_skip_value(ps);
            }
        } else if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            /* Child section inside LV: segments start with "segment" */
            if (wl_strlen(key) >= 7 && wl_memcmp(key, "segment", 7) == 0) {
                lvmcore_segment *seg = (lvmcore_segment *)arena_vec_push_empty(&seg_vec, arena);
                if (seg) {
                    seg->type = LVMCORE_SEG_OTHER; /* fail-closed default */
                    seg->stripe_count = 1;
                    parse_segment(ps, seg, arena);
                } else {
                    lv->segment_overflow = 1;
                    p_skip_section(ps);
                }
            } else {
                p_skip_section(ps);
            }
        } else {
            ps->pos++;
        }
    }

    lv->segments = (lvmcore_segment *)seg_vec.data;
    lv->segment_count = seg_vec.count;

    /* Sort segments by start_extent (insertion sort; non-recursive) */
    for (size_t i = 1; i < lv->segment_count; i++) {
        lvmcore_segment tmp = lv->segments[i];
        size_t j = i;
        while (j > 0 && lv->segments[j - 1].start_extent > tmp.start_extent) {
            lv->segments[j] = lv->segments[j - 1];
            j--;
        }
        lv->segments[j] = tmp;
    }
}

LVMCORE_NOINLINE static void parse_pv_body(parser *ps, lvmcore_physical_volume *pv) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '{') return;
    ps->pos++;

    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') {
            if (ps->pos < ps->len) ps->pos++;
            return;
        }
        char key[64];
        if (!p_ident(ps, key, sizeof(key))) {
            ps->pos++;
            continue;
        }
        p_skip(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == '=') {
            ps->pos++;
            p_skip(ps);
            if (wl_streq(key, "id")) {
                char v[64];
                if (p_string(ps, v, sizeof(v))) {
                    wl_strip_dashes(v);
                    wl_strcpy(pv->id, sizeof(pv->id), v);
                }
            } else if (wl_streq(key, "device")) {
                p_string(ps, pv->device, sizeof(pv->device));
            } else if (wl_streq(key, "pe_start")) {
                p_int(ps, &pv->pe_start);
            } else if (wl_streq(key, "pe_count")) {
                p_int(ps, &pv->pe_count);
            } else if (wl_streq(key, "dev_size")) {
                p_int(ps, &pv->dev_size_sectors);
            } else {
                p_skip_value(ps);
            }
        } else if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            p_skip_section(ps);
        } else {
            ps->pos++;
        }
    }
}

LVMCORE_NOINLINE static void parse_pvs_section(parser *ps, arena_vec *pvs_vec,
                                               lvmcore_arena *arena,
                                               lvmcore_volume_group *vg_out) {
    ps->pos++; /* consume opening brace */
    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') {
            if (ps->pos < ps->len) ps->pos++;
            break;
        }
        char pvkey[64];
        if (!p_ident(ps, pvkey, sizeof(pvkey))) {
            ps->pos++;
            continue;
        }
        p_skip(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            lvmcore_physical_volume *pv = (lvmcore_physical_volume *)arena_vec_push_empty(pvs_vec, arena);
            if (pv) {
                wl_strcpy(pv->key, sizeof(pv->key), pvkey);
                parse_pv_body(ps, pv);
            } else {
                vg_out->pv_overflow = 1;
                p_skip_section(ps);
            }
        } else {
            ps->pos++;
        }
    }
}

LVMCORE_NOINLINE static void parse_lvs_section(parser *ps, arena_vec *lvs_vec,
                                               lvmcore_arena *arena,
                                               lvmcore_volume_group *vg_out) {
    ps->pos++; /* consume opening brace */
    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') {
            if (ps->pos < ps->len) ps->pos++;
            break;
        }
        char lvname[64];
        if (!p_ident(ps, lvname, sizeof(lvname))) {
            ps->pos++;
            continue;
        }
        p_skip(ps);
        if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            lvmcore_logical_volume *lv = (lvmcore_logical_volume *)arena_vec_push_empty(lvs_vec, arena);
            if (lv) {
                wl_strcpy(lv->name, sizeof(lv->name), lvname);
                parse_lv_body(ps, lv, arena);

                /* Initial check for mappability */
                lv->mappable = 1;
                if (!lv->visible) {
                    lv->internal = 1;
                    lv->mappable = 0;
                    wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "hidden internal LV");
                }
                if (lv->segment_count == 0) {
                    lv->mappable = 0;
                    wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "no segments");
                }
                if (lv->segment_overflow) {
                    lv->mappable = 0;
                    wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "too many segments");
                }
                for (size_t k = 0; k < lv->segment_count && lv->mappable; k++) {
                    if (lv->segments[k].type != LVMCORE_SEG_STRIPED) {
                        lv->mappable = 0;
                        wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "unsupported segment type");
                    } else if (lv->segments[k].stripe_overflow) {
                        lv->mappable = 0;
                        wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), "too many stripes");
                    }
                }
            } else {
                vg_out->lv_overflow = 1;
                p_skip_section(ps);
            }
        } else {
            ps->pos++;
        }
    }
}

LVMCORE_NOINLINE static void parse_vg_second_pass(lvmcore_volume_group *vg_out) {
    /* Check snapshot origins and internal volume targets */
    for (size_t a = 0; a < vg_out->lv_count; a++) {
        for (size_t b = 0; b < vg_out->lv_count; b++) {
            for (size_t s = 0; s < vg_out->lvs[b].segment_count; s++) {
                const lvmcore_segment *seg = &vg_out->lvs[b].segments[s];
                if (seg->origin[0] && wl_streq(seg->origin, vg_out->lvs[a].name)) {
                    vg_out->lvs[a].mappable = 0;
                    wl_strcpy(vg_out->lvs[a].unsupported_reason,
                              sizeof(vg_out->lvs[a].unsupported_reason),
                              "logical volume is the origin of a snapshot");
                }
                for (size_t st = 0; st < seg->stripe_count_used; st++) {
                    if (wl_streq(seg->stripes[st].pv_key, vg_out->lvs[a].name)) {
                        vg_out->lvs[a].internal = 1;
                        if (vg_out->lvs[b].mappable) {
                            vg_out->lvs[b].mappable = 0;
                            wl_strcpy(vg_out->lvs[b].unsupported_reason,
                                      sizeof(vg_out->lvs[b].unsupported_reason),
                                      "segment maps onto logical volume, not a physical volume");
                        }
                    }
                }
            }
        }
    }

    if (vg_out->lv_overflow || vg_out->pv_overflow) {
        for (size_t a = 0; a < vg_out->lv_count; a++) {
            if (vg_out->lvs[a].mappable) {
                vg_out->lvs[a].mappable = 0;
                wl_strcpy(vg_out->lvs[a].unsupported_reason,
                          sizeof(vg_out->lvs[a].unsupported_reason),
                          vg_out->lv_overflow ? "volume group has too many LVs" : "volume group has too many PVs");
            }
        }
    }
}

lvmcore_status lvmcore_parse_vg_text(const char *text,
                                     size_t text_len,
                                     lvmcore_arena *arena,
                                     lvmcore_volume_group *vg_out,
                                     lvmcore_err *err) {
    if (!text || !arena || !vg_out) {
        wl_set_err(err, LVMCORE_ERR_INVALID_ARG, "invalid arguments to lvmcore_parse_vg_text");
        return LVMCORE_ERR_INVALID_ARG;
    }
    wl_zero(vg_out, sizeof(*vg_out));

    parser ps;
    p_init(&ps, text, text_len);
    bool found_vg = false;

    while (!p_eof(&ps)) {
        p_skip(&ps);
        if (p_eof(&ps)) break;
        char sec_name[64];
        if (!p_ident(&ps, sec_name, sizeof(sec_name))) {
            ps.pos++;
            continue;
        }
        p_skip(&ps);
        if (ps.pos < ps.len && ps.p[ps.pos] == '{') {
            ps.pos++;
            wl_strcpy(vg_out->name, sizeof(vg_out->name), sec_name);

            int depth = 1;
            bool has_id = false;
            bool has_extent = false;
            char id[33] = {0};
            uint64_t extent_size = 0;
            uint64_t seqno = 0;

            arena_vec pvs_vec;
            arena_vec lvs_vec;
            if (!arena_vec_init(&pvs_vec, arena, 8, sizeof(lvmcore_physical_volume)) ||
                !arena_vec_init(&lvs_vec, arena, 16, sizeof(lvmcore_logical_volume))) {
                wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted initializing VG arrays");
                return LVMCORE_ERR_SCRATCH_EXHAUSTED;
            }

            while (!p_eof(&ps) && depth > 0) {
                p_skip(&ps);
                if (p_eof(&ps)) break;
                if (ps.p[ps.pos] == '}') {
                    ps.pos++;
                    depth--;
                    if (depth == 0) break;
                    continue;
                }

                char child[64];
                size_t save = ps.pos;
                if (!p_ident(&ps, child, sizeof(child))) {
                    ps.pos = save + 1;
                    continue;
                }
                p_skip(&ps);

                if (ps.pos < ps.len && ps.p[ps.pos] == '=') {
                    ps.pos++;
                    p_skip(&ps);
                    if (wl_streq(child, "id")) {
                        char v[64];
                        if (p_string(&ps, v, sizeof(v))) {
                            wl_strip_dashes(v);
                            wl_strcpy(id, sizeof(id), v);
                            has_id = true;
                        }
                    } else if (wl_streq(child, "system_id")) {
                        p_string(&ps, vg_out->system_id, sizeof(vg_out->system_id));
                    } else if (wl_streq(child, "seqno")) {
                        p_int(&ps, &seqno);
                    } else if (wl_streq(child, "extent_size")) {
                        p_int(&ps, &extent_size);
                        has_extent = true;
                    } else {
                        p_skip_value(&ps);
                    }
                } else if (ps.pos < ps.len && ps.p[ps.pos] == '{') {
                    if (wl_streq(child, "physical_volumes")) {
                        parse_pvs_section(&ps, &pvs_vec, arena, vg_out);
                    } else if (wl_streq(child, "logical_volumes")) {
                        parse_lvs_section(&ps, &lvs_vec, arena, vg_out);
                    } else {
                        p_skip_section(&ps);
                    }
                } else {
                    ps.pos = save + 1;
                }
            }

            if (has_id && has_extent) {
                vg_out->pvs = (lvmcore_physical_volume *)pvs_vec.data;
                vg_out->pv_count = pvs_vec.count;
                vg_out->lvs = (lvmcore_logical_volume *)lvs_vec.data;
                vg_out->lv_count = lvs_vec.count;

                parse_vg_second_pass(vg_out);

                wl_strcpy(vg_out->id, sizeof(vg_out->id), id);
                vg_out->seqno = seqno;
                vg_out->extent_size_sectors = extent_size;
                found_vg = true;
                break;
            }
        } else if (ps.pos < ps.len && ps.p[ps.pos] == '=') {
            ps.pos++;
            p_skip_value(&ps);
        } else {
            ps.pos++;
        }
    }

    if (!found_vg) {
        wl_set_err(err, LVMCORE_ERR_NO_VG, "no volume group section found in metadata");
        return LVMCORE_ERR_NO_VG;
    }

    if (vg_out->extent_size_sectors == 0) {
        wl_set_err(err, LVMCORE_ERR_UNSUPPORTED, "volume group declares a zero extent size");
        return LVMCORE_ERR_UNSUPPORTED;
    }

    if (vg_out->pv_count == 0) {
        wl_set_err(err, LVMCORE_ERR_UNSUPPORTED, "volume group lists no physical volumes");
        return LVMCORE_ERR_UNSUPPORTED;
    }

    wl_set_err(err, LVMCORE_OK, NULL);
    return LVMCORE_OK;
}

/* ------------------------------------------------------------------ */
/* 5. High-level single PV and multi-PV entry points                   */
/* ------------------------------------------------------------------ */

lvmcore_status lvmcore_read_vg(lvmcore_byte_source *src,
                               uint32_t flags,
                               lvmcore_arena *arena,
                               lvmcore_volume_group *vg_out,
                               lvmcore_err *err) {
    if (!src || !arena || !vg_out) {
        wl_set_err(err, LVMCORE_ERR_INVALID_ARG, "invalid arguments to lvmcore_read_vg");
        return LVMCORE_ERR_INVALID_ARG;
    }
    wl_zero(vg_out, sizeof(*vg_out));

    /* Step 1: Discover LABELONE and parse PV header */
    lvmcore_pv_label label;
    lvmcore_status st = lvmcore_read_pv_label(src, arena, &label, err);
    if (st != LVMCORE_OK) return st;

    if (label.metadata_area_count == 0) {
        wl_set_err(err, LVMCORE_ERR_NO_VALID_MDA, "physical volume carries no metadata areas");
        return LVMCORE_ERR_NO_VALID_MDA;
    }

    /* Step 2 & 3: Iterate through metadata areas, take the first valid one */
    char *text = NULL;
    size_t text_len = 0;
    bool is_precommitted = false;
    bool got_text = false;
    lvmcore_err local_err;
    wl_zero(&local_err, sizeof(local_err));

    for (size_t i = 0; i < label.metadata_area_count; i++) {
        lvmcore_mda_header mda;
        if (lvmcore_read_mda_header(src, &label.metadata_areas[i], arena, &mda, &local_err) != LVMCORE_OK) {
            continue;
        }
        if (lvmcore_read_metadata_text(src, &label.metadata_areas[i], &mda, flags, arena,
                                       &text, &text_len, &is_precommitted, &local_err) == LVMCORE_OK) {
            got_text = true;
            break;
        }
    }

    if (!got_text) {
        wl_set_err(err, local_err.failed ? local_err.status : LVMCORE_ERR_NO_VALID_MDA,
                   local_err.failed ? local_err.message : "no readable metadata area found");
        return local_err.failed ? local_err.status : LVMCORE_ERR_NO_VALID_MDA;
    }

    /* Step 4: Parse metadata text */
    st = lvmcore_parse_vg_text(text, text_len, arena, vg_out, err);
    if (st != LVMCORE_OK) return st;

    if (is_precommitted) {
        vg_out->flags |= LVMCORE_VG_FLAG_PRECOMMITTED;
    }

    /* Step 5: Cross-check that metadata PV UUID matches the on-disk PV label UUID */
    bool pv_matched = false;
    for (size_t i = 0; i < vg_out->pv_count; i++) {
        if (wl_streq(vg_out->pvs[i].id, label.pv_uuid)) {
            pv_matched = true;
            break;
        }
    }
    if (!pv_matched) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_METADATA, "metadata PV UUID does not match label UUID");
        return LVMCORE_ERR_CORRUPT_METADATA;
    }

    wl_set_err(err, LVMCORE_OK, NULL);
    return LVMCORE_OK;
}

lvmcore_status lvmcore_read_vg_multi(lvmcore_byte_source **sources,
                                     size_t source_count,
                                     uint32_t flags,
                                     lvmcore_arena *arena,
                                     lvmcore_volume_group *vg_out,
                                     lvmcore_err *err) {
    if (!sources || source_count == 0 || !arena || !vg_out) {
        wl_set_err(err, LVMCORE_ERR_INVALID_ARG, "invalid arguments to lvmcore_read_vg_multi");
        return LVMCORE_ERR_INVALID_ARG;
    }
    wl_zero(vg_out, sizeof(*vg_out));

    bool have_candidate = false;
    lvmcore_volume_group *best_vg = (lvmcore_volume_group *)lvmcore_arena_alloc_zero(arena, sizeof(lvmcore_volume_group), 8);
    lvmcore_volume_group *curr_vg = (lvmcore_volume_group *)lvmcore_arena_alloc_zero(arena, sizeof(lvmcore_volume_group), 8);
    if (!best_vg || !curr_vg) {
        wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted in lvmcore_read_vg_multi");
        return LVMCORE_ERR_SCRATCH_EXHAUSTED;
    }

    bool seqno_mismatch = false;

    for (size_t s = 0; s < source_count; s++) {
        if (!sources[s]) continue;
        lvmcore_err local_err;
        wl_zero(&local_err, sizeof(local_err));

        if (lvmcore_read_vg(sources[s], flags, arena, curr_vg, &local_err) != LVMCORE_OK) {
            continue;
        }

        if (!have_candidate) {
            *best_vg = *curr_vg;
            have_candidate = true;
        } else {
            /* Verify it belongs to the same VG ID */
            if (wl_streq(best_vg->id, curr_vg->id)) {
                if (curr_vg->seqno > best_vg->seqno) {
                    *best_vg = *curr_vg;
                    seqno_mismatch = true;
                } else if (curr_vg->seqno < best_vg->seqno) {
                    seqno_mismatch = true;
                }
            }
        }
    }

    if (!have_candidate) {
        wl_set_err(err, LVMCORE_ERR_NO_VG, "failed to read volume group from any provided source");
        return LVMCORE_ERR_NO_VG;
    }

    if (seqno_mismatch) {
        best_vg->flags |= LVMCORE_VG_FLAG_SEQNO_MISMATCH;
    }

    *vg_out = *best_vg;
    wl_set_err(err, LVMCORE_OK, NULL);
    return LVMCORE_OK;
}

/* ------------------------------------------------------------------ */
/* Model Lookup Helpers                                               */
/* ------------------------------------------------------------------ */

const lvmcore_physical_volume *lvmcore_vg_find_pv(const lvmcore_volume_group *vg,
                                                  const char *key) {
    if (!vg || !key) return NULL;
    for (size_t i = 0; i < vg->pv_count; i++) {
        if (wl_streq(vg->pvs[i].key, key)) return &vg->pvs[i];
    }
    return NULL;
}

const lvmcore_physical_volume *lvmcore_vg_find_pv_by_uuid(const lvmcore_volume_group *vg,
                                                          const char *uuid) {
    if (!vg || !uuid) return NULL;
    for (size_t i = 0; i < vg->pv_count; i++) {
        if (wl_streq(vg->pvs[i].id, uuid)) return &vg->pvs[i];
    }
    return NULL;
}

const lvmcore_logical_volume *lvmcore_vg_find_lv(const lvmcore_volume_group *vg,
                                                 const char *name) {
    if (!vg || !name) return NULL;
    for (size_t i = 0; i < vg->lv_count; i++) {
        if (wl_streq(vg->lvs[i].name, name)) return &vg->lvs[i];
    }
    return NULL;
}

uint64_t lvmcore_lv_extent_count(const lvmcore_logical_volume *lv) {
    if (!lv) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < lv->segment_count; i++) {
        if (UINT64_MAX - total < lv->segments[i].extent_count) {
            return UINT64_MAX; /* Saturate on overflow */
        }
        total += lv->segments[i].extent_count;
    }
    return total;
}
