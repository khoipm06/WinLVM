/*
 * parser.c — freestanding C11 LVM2 metadata parser.
 *
 * Implements non-recursive, bounds-checked parsing with zero dynamic
 * allocations. All state lives in caller-supplied scratch arena.
 */
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

static void wl_memswap(void *a, void *b, size_t n) {
    uint8_t *pa = (uint8_t *)a;
    uint8_t *pb = (uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        uint8_t t = pa[i];
        pa[i] = pb[i];
        pb[i] = t;
    }
}

static int wl_ends_with(const char *s, const char *suffix) {
    size_t sl = wl_strlen(s);
    size_t xl = wl_strlen(suffix);
    if (sl < xl) return 0;
    return wl_memcmp(s + sl - xl, suffix, xl) == 0;
}

static int wl_is_snapshot_wrapper_name(const char *name) {
    if (!name) return 0;
    size_t len = wl_strlen(name);
    if (len < 9 || wl_memcmp(name, "snapshot", 8) != 0) return 0;
    for (size_t i = 8; i < len; i++) {
        if (name[i] < '0' || name[i] > '9') return 0;
    }
    return 1;
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

static lvmcore_status parse_area_list(const uint8_t *buf, size_t bufsz, size_t *cursor,
                                      arena_vec *out_vec, lvmcore_arena *arena,
                                      lvmcore_err *err) {
    for (;;) {
        if (*cursor > bufsz || bufsz - *cursor < 16u) {
            wl_set_err(err, LVMCORE_ERR_CORRUPT_LABEL, "PV header truncated in disk-area list");
            return LVMCORE_ERR_CORRUPT_LABEL;
        }
        uint64_t off = le64(buf + *cursor);
        uint64_t sz  = le64(buf + *cursor + 8);
        *cursor += 16u;
        if (off == 0 && sz == 0) {
            /* Sentinel reached */
            return LVMCORE_OK;
        }
        lvmcore_disk_area da;
        da.offset = off;
        da.size = sz;
        if (!arena_vec_push(out_vec, arena, &da)) {
            wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted parsing disk areas");
            return LVMCORE_ERR_SCRATCH_EXHAUSTED;
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

        /*
         * pv_header begins at data_off inside the 512-byte label sector and
         * carries a 32-byte pv_uuid + 8-byte device_size (40 bytes) followed
         * by two (0,0)-terminated 16-byte disk_locn lists, all within [off, off + 512).
         */
        uint32_t data_off = le32(p + 20);
        if (data_off < 32u || data_off > LVMCORE_LABEL_SIZE - 40u) {
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

        size_t cursor = 40u;
        size_t pvh_max = (size_t)LVMCORE_LABEL_SIZE - (size_t)data_off;
        lvmcore_status area_st = parse_area_list(pvh, pvh_max, &cursor, &data_areas_vec, arena, err);
        if (area_st != LVMCORE_OK) {
            return area_st;
        }
        area_st = parse_area_list(pvh, pvh_max, &cursor, &meta_areas_vec, arena, err);
        if (area_st != LVMCORE_OK) {
            return area_st;
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

    /*
     * On-disk mda_header is 40 bytes followed by packed 24-byte raw_locn slots:
     *   Slot 0 (committed):    bytes 40..63 (offset 40..47, size 48..55, checksum 56..59, flags 60..63)
     *   Slot 1 (precommitted): bytes 64..87 (offset 64..71, size 72..79, checksum 80..83, flags 84..87)
     */
    mda_out->raw_locns[0].offset   = le64(buf + 40);
    mda_out->raw_locns[0].size     = le64(buf + 48);
    mda_out->raw_locns[0].checksum = le32(buf + 56);
    mda_out->raw_locns[0].flags    = le32(buf + 60);

    mda_out->raw_locns[1].offset   = le64(buf + 64);
    mda_out->raw_locns[1].size     = le64(buf + 72);
    mda_out->raw_locns[1].checksum = le32(buf + 80);
    mda_out->raw_locns[1].flags    = le32(buf + 84);

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

    if (area->size < LVMCORE_MDA_HEADER_SIZE ||
        area->offset > src->size ||
        area->size > src->size - area->offset) {
        wl_set_err(err, LVMCORE_ERR_CORRUPT_MDA, "metadata area lies outside device boundaries");
        return LVMCORE_ERR_CORRUPT_MDA;
    }

    /* LVM2 marks ignored MDAs (--metadataignore y) via RAW_LOCN_IGNORED on slot 0. */
    if ((mda->raw_locns[0].flags & LVMCORE_RAW_LOCN_IGNORED) != 0) {
        wl_set_err(err, LVMCORE_ERR_NO_VALID_MDA, "metadata area is marked ignored");
        return LVMCORE_ERR_NO_VALID_MDA;
    }

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

#define WL_STAT_VISIBLE     (1u << 0)
#define WL_STAT_WRITE       (1u << 1)
#define WL_STAT_MISSING     (1u << 2)
#define WL_STAT_ALLOCATABLE (1u << 3)
#define WL_STAT_EXPORTED    (1u << 4)
#define WL_STAT_RESIZEABLE  (1u << 5)
#define WL_STAT_NONEMPTY    (1u << 6)

LVMCORE_NOINLINE static uint32_t parse_status_list(parser *ps) {
    uint32_t mask = 0;
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '[') {
        p_skip_value(ps);
        return 0;
    }
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
            mask |= WL_STAT_NONEMPTY;
            if (wl_streq(s, "VISIBLE")) mask |= WL_STAT_VISIBLE;
            else if (wl_streq(s, "WRITE")) mask |= WL_STAT_WRITE;
            else if (wl_streq(s, "MISSING")) mask |= WL_STAT_MISSING;
            else if (wl_streq(s, "ALLOCATABLE")) mask |= WL_STAT_ALLOCATABLE;
            else if (wl_streq(s, "EXPORTED")) mask |= WL_STAT_EXPORTED;
            else if (wl_streq(s, "RESIZEABLE")) mask |= WL_STAT_RESIZEABLE;
        } else {
            ps->pos++;
        }
    }
    return mask;
}

LVMCORE_NOINLINE static void parse_tag_list(parser *ps, lvmcore_arena *arena,
                                            lvmcore_tag **out_tags,
                                            size_t *out_count) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '[') {
        p_skip_value(ps);
        return;
    }
    ps->pos++;
    arena_vec vec;
    if (!arena_vec_init(&vec, arena, 4, sizeof(lvmcore_tag))) {
        ps->pos--;
        p_skip_value(ps);
        return;
    }
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
        if (ps->p[ps->pos] == '"') {
            lvmcore_tag *item = (lvmcore_tag *)arena_vec_push_empty(&vec, arena);
            if (!item) {
                char discard[64];
                p_string(ps, discard, sizeof(discard));
            } else if (!p_string(ps, item->name, sizeof(item->name))) {
                vec.count--;
                break;
            }
        } else {
            ps->pos++;
        }
    }
    *out_tags = (lvmcore_tag *)vec.data;
    *out_count = vec.count;
}

LVMCORE_NOINLINE static void parse_stripes(parser *ps, lvmcore_segment *seg, lvmcore_arena *arena) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '[') return;
    ps->pos++;

    arena_vec stripes_vec;
    if (!arena_vec_init(&stripes_vec, arena, 4, sizeof(lvmcore_stripe))) {
        seg->stripe_overflow = 1;
        ps->pos--;
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
            lvmcore_stripe *st = (lvmcore_stripe *)arena_vec_push_empty(&stripes_vec, arena);
            if (!st) {
                char discard[64];
                uint64_t dummy = 0;
                seg->stripe_overflow = 1;
                if (!p_string(ps, discard, sizeof(discard))) break;
                p_skip(ps);
                if (ps->pos < ps->len && ps->p[ps->pos] == ',') ps->pos++;
                if (!p_int(ps, &dummy)) break;
                continue;
            }
            if (!p_string(ps, st->pv_key, sizeof(st->pv_key))) {
                stripes_vec.count--;
                break;
            }
            p_skip(ps);
            if (ps->pos < ps->len && ps->p[ps->pos] == ',') ps->pos++;
            if (!p_int(ps, &st->start_extent)) {
                stripes_vec.count--;
                break;
            }
        } else {
            ps->pos++;
        }
    }

    seg->stripes = (lvmcore_stripe *)stripes_vec.data;
    seg->stripe_count_used = stripes_vec.count;
}

LVMCORE_NOINLINE static void parse_raids(parser *ps, lvmcore_segment *seg, lvmcore_arena *arena) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '[') {
        p_skip_value(ps);
        return;
    }
    ps->pos++;

    arena_vec vec;
    if (!arena_vec_init(&vec, arena, 4, sizeof(lvmcore_raid_leg))) {
        seg->stripe_overflow = 1;
        ps->pos--;
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
            lvmcore_raid_leg *leg = (lvmcore_raid_leg *)arena_vec_push_empty(&vec, arena);
            if (!leg) {
                char discard[64];
                seg->stripe_overflow = 1;
                if (!p_string(ps, discard, sizeof(discard))) break;
                p_skip(ps);
                if (ps->pos < ps->len && ps->p[ps->pos] == ',') ps->pos++;
                if (!p_string(ps, discard, sizeof(discard))) break;
                continue;
            }
            if (!p_string(ps, leg->meta_lv, sizeof(leg->meta_lv))) {
                vec.count--;
                break;
            }
            p_skip(ps);
            if (ps->pos < ps->len && ps->p[ps->pos] == ',') ps->pos++;
            if (!p_string(ps, leg->image_lv, sizeof(leg->image_lv))) {
                vec.count--;
                break;
            }
        } else {
            ps->pos++;
        }
    }

    seg->raid_legs = (lvmcore_raid_leg *)vec.data;
    seg->raid_leg_count = vec.count;
}

LVMCORE_NOINLINE static void parse_mirrors(parser *ps, lvmcore_segment *seg, lvmcore_arena *arena) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '[') {
        p_skip_value(ps);
        return;
    }
    ps->pos++;

    arena_vec vec;
    if (!arena_vec_init(&vec, arena, 4, sizeof(lvmcore_raid_leg))) {
        seg->stripe_overflow = 1;
        ps->pos--;
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
            lvmcore_raid_leg *leg = (lvmcore_raid_leg *)arena_vec_push_empty(&vec, arena);
            if (!leg) {
                char discard[64];
                uint64_t dummy = 0;
                seg->stripe_overflow = 1;
                if (!p_string(ps, discard, sizeof(discard))) break;
                p_skip(ps);
                if (ps->pos < ps->len && ps->p[ps->pos] == ',') ps->pos++;
                if (!p_int(ps, &dummy)) break;
                continue;
            }
            if (!p_string(ps, leg->image_lv, sizeof(leg->image_lv))) {
                vec.count--;
                break;
            }
            p_skip(ps);
            if (ps->pos < ps->len && ps->p[ps->pos] == ',') ps->pos++;
            if (!p_int(ps, &leg->start_extent)) {
                vec.count--;
                break;
            }
        } else {
            ps->pos++;
        }
    }

    seg->raid_legs = (lvmcore_raid_leg *)vec.data;
    seg->raid_leg_count = vec.count;
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
    bool has_stripe_count = false;

    for (;;) {
        p_skip(ps);
        if (ps->pos >= ps->len || ps->p[ps->pos] == '}') {
            if (ps->pos < ps->len) ps->pos++;
            break;
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
            size_t val_start = ps->pos;
            if (wl_streq(key, "start_extent")) {
                p_int(ps, &seg->start_extent);
            } else if (wl_streq(key, "extent_count")) {
                p_int(ps, &seg->extent_count);
            } else if (wl_streq(key, "stripe_count")) {
                p_int(ps, &seg->stripe_count);
                has_stripe_count = true;
            } else if (wl_streq(key, "stripe_size")) {
                p_int(ps, &seg->stripe_size_sectors);
            } else if (wl_streq(key, "origin")) {
                p_string(ps, seg->origin, sizeof(seg->origin));
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "metadata")) {
                p_string(ps, seg->pool_metadata, sizeof(seg->pool_metadata));
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "pool") || wl_streq(key, "thin_pool")) {
                p_string(ps, seg->pool_data, sizeof(seg->pool_data));
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "cow_store") || wl_streq(key, "cow")) {
                p_string(ps, seg->cow_store, sizeof(seg->cow_store));
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "mirror_log")) {
                p_string(ps, seg->mirror_log, sizeof(seg->mirror_log));
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "discards")) {
                if (!p_string(ps, seg->discards, sizeof(seg->discards))) {
                    p_ident(ps, seg->discards, sizeof(seg->discards));
                }
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "chunk_size")) {
                p_int(ps, &seg->chunk_size_sectors);
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "transaction_id")) {
                if (p_int(ps, &seg->transaction_id)) seg->has_transaction_id = 1;
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "device_id")) {
                if (p_int(ps, &seg->device_id)) seg->has_device_id = 1;
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "zero_new_blocks")) {
                uint64_t zv = 0;
                if (p_int(ps, &zv)) {
                    seg->zero_new_blocks = (zv != 0) ? 1 : 0;
                    seg->has_zero_new_blocks = 1;
                }
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "region_size")) {
                p_int(ps, &seg->region_size_sectors);
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "device_count") || wl_streq(key, "raid_count") ||
                       wl_streq(key, "mirror_count") || wl_streq(key, "area_count")) {
                p_int(ps, &seg->device_count);
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "type")) {
                if (p_string(ps, seg->seg_type_name, sizeof(seg->seg_type_name)) ||
                    p_ident(ps, seg->seg_type_name, sizeof(seg->seg_type_name))) {
                    if (wl_streq(seg->seg_type_name, "striped") ||
                        wl_streq(seg->seg_type_name, "linear")) {
                        seg->type = LVMCORE_SEG_STRIPED;
                    } else if (wl_streq(seg->seg_type_name, "thin-pool")) {
                        seg->type = LVMCORE_SEG_THIN_POOL;
                    } else if (wl_streq(seg->seg_type_name, "thin")) {
                        seg->type = LVMCORE_SEG_THIN;
                    } else if (wl_streq(seg->seg_type_name, "snapshot")) {
                        seg->type = LVMCORE_SEG_SNAPSHOT;
                    } else if (wl_streq(seg->seg_type_name, "raid1")) {
                        seg->type = LVMCORE_SEG_RAID1;
                    } else if (wl_streq(seg->seg_type_name, "mirror")) {
                        seg->type = LVMCORE_SEG_MIRROR;
                    } else {
                        seg->type = LVMCORE_SEG_OTHER;
                    }
                }
            } else if (wl_streq(key, "stripes")) {
                parse_stripes(ps, seg, arena);
            } else if (wl_streq(key, "raids")) {
                parse_raids(ps, seg, arena);
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else if (wl_streq(key, "mirrors")) {
                parse_mirrors(ps, seg, arena);
                seg_append_raw(ps, seg, arena, key_start, val_start);
            } else {
                /* Unmapped key: capture raw "key = value" span for SEG_OTHER re-emit. */
                p_skip_value(ps);
                seg_append_raw(ps, seg, arena, key_start, val_start);
            }
        } else if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            p_skip_section(ps);
        } else {
            ps->pos++;
        }
    }
    if (seg->type == LVMCORE_SEG_STRIPED && !has_stripe_count) {
        seg->stripe_count = 1;
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

    bool has_write = false;

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
                uint32_t st_mask = parse_status_list(ps);
                if (st_mask & WL_STAT_VISIBLE) {
                    lv->visible = 1;
                    lv->status_visible = 1;
                }
                if (st_mask & WL_STAT_WRITE) has_write = true;
            } else if (wl_streq(key, "flags")) {
                parse_tag_list(ps, arena, &lv->flags, &lv->flag_count);
            } else if (wl_streq(key, "tags")) {
                parse_tag_list(ps, arena, &lv->tags, &lv->tag_count);
            } else {
                p_skip_value(ps);
            }
        } else if (ps->pos < ps->len && ps->p[ps->pos] == '{') {
            /* Child section inside LV: segments start with "segment" */
            if (wl_strlen(key) >= 7 && wl_memcmp(key, "segment", 7) == 0) {
                lvmcore_segment *seg = (lvmcore_segment *)arena_vec_push_empty(&seg_vec, arena);
                if (seg) {
                    seg->type = LVMCORE_SEG_OTHER; /* fail-closed default */
                    seg->stripe_count = 0;
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

    lv->read_only = has_write ? 0 : 1;
    lv->segments = (lvmcore_segment *)seg_vec.data;
    lv->segment_count = seg_vec.count;

    /* Sort segments by start_extent using in-place swaps (zero stack copy). */
    for (size_t i = 1; i < lv->segment_count; i++) {
        size_t j = i;
        while (j > 0 && lv->segments[j - 1].start_extent > lv->segments[j].start_extent) {
            wl_memswap(&lv->segments[j - 1], &lv->segments[j], sizeof(lvmcore_segment));
            j--;
        }
    }
}

LVMCORE_NOINLINE static void parse_pv_body(parser *ps, lvmcore_physical_volume *pv, lvmcore_arena *arena) {
    p_skip(ps);
    if (ps->pos >= ps->len || ps->p[ps->pos] != '{') return;
    ps->pos++;

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
            if (wl_streq(key, "id")) {
                char v[64];
                if (p_string(ps, v, sizeof(v))) {
                    wl_strip_dashes(v);
                    wl_strcpy(pv->id, sizeof(pv->id), v);
                }
            } else if (wl_streq(key, "device")) {
                p_string(ps, pv->device, sizeof(pv->device));
            } else if (wl_streq(key, "status")) {
                uint32_t st_mask = parse_status_list(ps);
                if (st_mask & WL_STAT_MISSING) pv->missing = 1;
                if ((st_mask & WL_STAT_NONEMPTY) && !(st_mask & WL_STAT_ALLOCATABLE)) {
                    pv->not_allocatable = 1;
                }
            } else if (wl_streq(key, "flags")) {
                parse_tag_list(ps, arena, &pv->flags, &pv->flag_count);
                for (size_t fi = 0; fi < pv->flag_count; fi++) {
                    if (wl_streq(pv->flags[fi].name, "MISSING")) pv->missing = 1;
                }
            } else if (wl_streq(key, "tags")) {
                parse_tag_list(ps, arena, &pv->tags, &pv->tag_count);
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
                parse_pv_body(ps, pv, arena);
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

                /* Initial eligibility check; second pass resolves sub-LV references. */
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
                    if (lv->segments[k].type == LVMCORE_SEG_OTHER) {
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

static void mark_lv_unmappable(lvmcore_logical_volume *lv, const char *reason) {
    lv->mappable = 0;
    wl_strcpy(lv->unsupported_reason, sizeof(lv->unsupported_reason), reason);
}

static lvmcore_logical_volume *vg_find_lv_mut(lvmcore_volume_group *vg, const char *name) {
    if (!vg || !vg->lvs || !name || !name[0]) return NULL;
    for (size_t i = 0; i < vg->lv_count; i++) {
        if (wl_streq(vg->lvs[i].name, name)) return &vg->lvs[i];
    }
    return NULL;
}

LVMCORE_NOINLINE static const char *validate_linear_striped_lv(
        const lvmcore_volume_group *vg,
        const lvmcore_logical_volume *lv,
        int require_present_pvs) {
    if (!lv || lv->segment_count == 0 || !lv->segments) {
        return "no segments";
    }
    if (lv->declared_segment_count > 0 &&
        (uint64_t)lv->segment_count != lv->declared_segment_count) {
        return "segment count mismatch";
    }

    uint64_t expected_next_extent = 0;
    for (size_t i = 0; i < lv->segment_count; i++) {
        const lvmcore_segment *seg = &lv->segments[i];

        if (seg->extent_count == 0) {
            return "segment has zero extent count";
        }
        if (seg->start_extent != expected_next_extent) {
            return seg->start_extent < expected_next_extent
                       ? "overlapping segments"
                       : "non-contiguous segments";
        }
        if (UINT64_MAX - expected_next_extent < seg->extent_count) {
            return "segment extent range overflow";
        }
        expected_next_extent += seg->extent_count;

        if (seg->type != LVMCORE_SEG_STRIPED) {
            return "unsupported segment type";
        }
        if (seg->stripe_overflow) {
            return "too many stripes";
        }
        if (seg->stripe_count < 1) {
            return "invalid stripe count";
        }
        if (!seg->stripes || (uint64_t)seg->stripe_count_used != seg->stripe_count) {
            return "stripe count mismatch";
        }
        if (seg->stripe_count > 1) {
            if (seg->stripe_size_sectors == 0 ||
                (seg->stripe_size_sectors & (seg->stripe_size_sectors - 1u)) != 0) {
                return "invalid stripe size";
            }
            if ((seg->extent_count % seg->stripe_count) != 0) {
                return "extent count not divisible by stripe count";
            }
        }

        uint64_t per_stripe_extents = seg->extent_count / seg->stripe_count;
        for (size_t s = 0; s < seg->stripe_count_used; s++) {
            const lvmcore_physical_volume *pv = lvmcore_vg_find_pv(vg, seg->stripes[s].pv_key);
            if (!pv) {
                return "stripe references unknown physical volume";
            }
            if (require_present_pvs && pv->missing) {
                return "stripe references missing physical volume";
            }
            if (seg->stripes[s].start_extent > pv->pe_count ||
                per_stripe_extents > pv->pe_count - seg->stripes[s].start_extent) {
                return "stripe exceeds physical volume extent bounds";
            }
        }
    }
    return NULL;
}

LVMCORE_NOINLINE static void validate_lv_segments(const lvmcore_volume_group *vg,
                                                  lvmcore_logical_volume *lv) {
    if (!lv->mappable) return;

    if (lv->segment_count == 0 || !lv->segments) {
        mark_lv_unmappable(lv, "no segments");
        return;
    }
    if (lv->declared_segment_count > 0 &&
        (uint64_t)lv->segment_count != lv->declared_segment_count) {
        mark_lv_unmappable(lv, "segment count mismatch");
        return;
    }

    /* Classic LVM2 COW LV exposed as user snapshot via snapshot_wrapper. */
    if (lv->is_snapshot_cow && lv->snapshot_wrapper[0]) {
        const lvmcore_logical_volume *wrap = lvmcore_vg_find_lv(vg, lv->snapshot_wrapper);
        if (!wrap || wrap->segment_count == 0 || wrap->segments[0].type != LVMCORE_SEG_SNAPSHOT) {
            mark_lv_unmappable(lv, "invalid snapshot wrapper");
            return;
        }
        const lvmcore_segment *wseg = &wrap->segments[0];
        if (wseg->chunk_size_sectors == 0 ||
            (wseg->chunk_size_sectors & (wseg->chunk_size_sectors - 1u)) != 0) {
            mark_lv_unmappable(lv, "invalid snapshot chunk size");
            return;
        }
        const lvmcore_logical_volume *orig = lvmcore_vg_find_lv(vg, wseg->origin);
        if (!orig) {
            mark_lv_unmappable(lv, "snapshot origin not found");
            return;
        }
        const char *oerr = validate_linear_striped_lv(vg, orig, 1);
        if (oerr) {
            mark_lv_unmappable(lv, oerr);
            return;
        }
        const char *cerr = validate_linear_striped_lv(vg, lv, 1);
        if (cerr) {
            mark_lv_unmappable(lv, cerr);
        }
        return;
    }

    lvmcore_seg_type first_type = lv->segments[0].type;

    if (first_type == LVMCORE_SEG_STRIPED) {
        const char *err = validate_linear_striped_lv(vg, lv, 1);
        if (err) mark_lv_unmappable(lv, err);
        return;
    }

    if (first_type == LVMCORE_SEG_THIN) {
        uint64_t expected_next = 0;
        for (size_t i = 0; i < lv->segment_count; i++) {
            const lvmcore_segment *seg = &lv->segments[i];
            if (seg->type != LVMCORE_SEG_THIN || seg->extent_count == 0 ||
                seg->start_extent != expected_next || !seg->has_device_id ||
                !seg->pool_data[0]) {
                mark_lv_unmappable(lv, "invalid thin segment");
                return;
            }
            if (UINT64_MAX - expected_next < seg->extent_count) {
                mark_lv_unmappable(lv, "segment extent range overflow");
                return;
            }
            expected_next += seg->extent_count;

            const lvmcore_logical_volume *pool = lvmcore_vg_find_lv(vg, seg->pool_data);
            if (!pool || pool->segment_count == 0 ||
                pool->segments[0].type != LVMCORE_SEG_THIN_POOL) {
                mark_lv_unmappable(lv, "thin pool not found");
                return;
            }
            const lvmcore_segment *pseg = &pool->segments[0];
            if (pseg->chunk_size_sectors == 0 ||
                (pseg->chunk_size_sectors & (pseg->chunk_size_sectors - 1u)) != 0) {
                mark_lv_unmappable(lv, "invalid thin pool chunk size");
                return;
            }
            const lvmcore_logical_volume *tmeta = lvmcore_vg_find_lv(vg, pseg->pool_metadata);
            const lvmcore_logical_volume *tdata = lvmcore_vg_find_lv(vg, pseg->pool_data);
            const char *merr = validate_linear_striped_lv(vg, tmeta, 1);
            if (merr) {
                mark_lv_unmappable(lv, merr);
                return;
            }
            const char *derr = validate_linear_striped_lv(vg, tdata, 1);
            if (derr) {
                mark_lv_unmappable(lv, derr);
                return;
            }
        }
        return;
    }

    if (first_type == LVMCORE_SEG_SNAPSHOT) {
        if (lv->segment_count != 1) {
            mark_lv_unmappable(lv, "multi-segment snapshot unsupported");
            return;
        }
        const lvmcore_segment *seg = &lv->segments[0];
        if (seg->extent_count == 0 || seg->chunk_size_sectors == 0 ||
            (seg->chunk_size_sectors & (seg->chunk_size_sectors - 1u)) != 0) {
            mark_lv_unmappable(lv, "invalid snapshot geometry");
            return;
        }
        const lvmcore_logical_volume *orig = lvmcore_vg_find_lv(vg, seg->origin);
        const lvmcore_logical_volume *cow = lvmcore_vg_find_lv(vg, seg->cow_store);
        const char *oerr = validate_linear_striped_lv(vg, orig, 1);
        if (oerr) {
            mark_lv_unmappable(lv, oerr);
            return;
        }
        const char *cerr = validate_linear_striped_lv(vg, cow, 1);
        if (cerr) {
            mark_lv_unmappable(lv, cerr);
        }
        return;
    }

    if (first_type == LVMCORE_SEG_RAID1 || first_type == LVMCORE_SEG_MIRROR) {
        uint64_t expected_next = 0;
        for (size_t i = 0; i < lv->segment_count; i++) {
            const lvmcore_segment *seg = &lv->segments[i];
            if (seg->type != first_type || seg->extent_count == 0 ||
                seg->start_extent != expected_next || seg->raid_leg_count == 0 ||
                !seg->raid_legs) {
                mark_lv_unmappable(lv, "invalid raid1/mirror segment");
                return;
            }
            if (UINT64_MAX - expected_next < seg->extent_count) {
                mark_lv_unmappable(lv, "segment extent range overflow");
                return;
            }
            expected_next += seg->extent_count;

            size_t healthy_legs = 0;
            for (size_t l = 0; l < seg->raid_leg_count; l++) {
                const lvmcore_logical_volume *img =
                    lvmcore_vg_find_lv(vg, seg->raid_legs[l].image_lv);
                if (!img) continue;
                uint64_t img_exts = lvmcore_lv_extent_count(img);
                if (img_exts == UINT64_MAX ||
                    seg->raid_legs[l].start_extent > img_exts ||
                    img_exts - seg->raid_legs[l].start_extent < seg->extent_count) {
                    mark_lv_unmappable(lv, "raid1/mirror leg smaller than LV");
                    return;
                }
                if (validate_linear_striped_lv(vg, img, 1) == NULL) {
                    healthy_legs++;
                }
            }
            if (healthy_legs == 0) {
                mark_lv_unmappable(lv, "no healthy raid1/mirror leg available");
                return;
            }
        }
        return;
    }

    mark_lv_unmappable(lv, "unsupported segment type");
}

LVMCORE_NOINLINE static void parse_vg_second_pass(lvmcore_volume_group *vg_out) {
    /* Pass 1: Resolve sub-LV references, thin-pool containers, and snapshot relationships. */
    for (size_t b = 0; b < vg_out->lv_count; b++) {
        lvmcore_logical_volume *lv = &vg_out->lvs[b];
        if (wl_ends_with(lv->name, "_pmspare")) {
            lv->internal = 1;
            mark_lv_unmappable(lv, "hidden internal LV");
        }
        for (size_t s = 0; s < lv->segment_count; s++) {
            const lvmcore_segment *seg = &lv->segments[s];

            if (seg->type == LVMCORE_SEG_THIN_POOL) {
                lv->is_thin_pool = 1;
                mark_lv_unmappable(lv, "thin-pool container LV");
                lvmcore_logical_volume *tm = vg_find_lv_mut(vg_out, seg->pool_metadata);
                lvmcore_logical_volume *td = vg_find_lv_mut(vg_out, seg->pool_data);
                if (tm) { tm->internal = 1; mark_lv_unmappable(tm, "hidden internal LV"); }
                if (td) { td->internal = 1; mark_lv_unmappable(td, "hidden internal LV"); }
            } else if (seg->type == LVMCORE_SEG_SNAPSHOT) {
                lvmcore_logical_volume *orig = vg_find_lv_mut(vg_out, seg->origin);
                lvmcore_logical_volume *cow = vg_find_lv_mut(vg_out, seg->cow_store);
                if (orig) {
                    orig->is_snapshot_origin = 1;
                }
                if (wl_is_snapshot_wrapper_name(lv->name) && cow &&
                    !wl_ends_with(cow->name, "_cow")) {
                    /* Classic LVM2 wrapper: snapshot0 is internal, cow_store LV is user-visible. */
                    lv->internal = 1;
                    mark_lv_unmappable(lv, "internal snapshot wrapper");
                    cow->is_snapshot_cow = 1;
                    wl_strcpy(cow->snapshot_wrapper, sizeof(cow->snapshot_wrapper), lv->name);
                    cow->visible = 1;
                    cow->internal = 0;
                    cow->mappable = 1;
                    cow->unsupported_reason[0] = '\0';
                } else {
                    if (cow) {
                        cow->internal = 1;
                        mark_lv_unmappable(cow, "hidden internal LV");
                    }
                }
            } else if (seg->type == LVMCORE_SEG_RAID1 || seg->type == LVMCORE_SEG_MIRROR) {
                for (size_t l = 0; l < seg->raid_leg_count; l++) {
                    lvmcore_logical_volume *mlv = vg_find_lv_mut(vg_out, seg->raid_legs[l].meta_lv);
                    lvmcore_logical_volume *ilv = vg_find_lv_mut(vg_out, seg->raid_legs[l].image_lv);
                    if (mlv) { mlv->internal = 1; mark_lv_unmappable(mlv, "hidden internal LV"); }
                    if (ilv) { ilv->internal = 1; mark_lv_unmappable(ilv, "hidden internal LV"); }
                }
                if (seg->mirror_log[0]) {
                    lvmcore_logical_volume *loglv = vg_find_lv_mut(vg_out, seg->mirror_log);
                    if (loglv) { loglv->internal = 1; mark_lv_unmappable(loglv, "hidden internal LV"); }
                }
            } else if (seg->type == LVMCORE_SEG_OTHER) {
                /* Mark any sub-LVs referenced by unsupported segment types (e.g., raid5, cache) internal. */
                for (size_t l = 0; l < seg->raid_leg_count; l++) {
                    lvmcore_logical_volume *mlv = vg_find_lv_mut(vg_out, seg->raid_legs[l].meta_lv);
                    lvmcore_logical_volume *ilv = vg_find_lv_mut(vg_out, seg->raid_legs[l].image_lv);
                    if (mlv) { mlv->internal = 1; mark_lv_unmappable(mlv, "hidden internal LV"); }
                    if (ilv) { ilv->internal = 1; mark_lv_unmappable(ilv, "hidden internal LV"); }
                }
                if (seg->pool_metadata[0]) {
                    lvmcore_logical_volume *mlv = vg_find_lv_mut(vg_out, seg->pool_metadata);
                    if (mlv) { mlv->internal = 1; mark_lv_unmappable(mlv, "hidden internal LV"); }
                }
                if (seg->pool_data[0]) {
                    lvmcore_logical_volume *dlv = vg_find_lv_mut(vg_out, seg->pool_data);
                    if (dlv) { dlv->internal = 1; mark_lv_unmappable(dlv, "hidden internal LV"); }
                }
            }

            if (seg->stripes) {
                for (size_t st = 0; st < seg->stripe_count_used; st++) {
                    lvmcore_logical_volume *target = vg_find_lv_mut(vg_out, seg->stripes[st].pv_key);
                    if (target) {
                        target->internal = 1;
                        if (lv->mappable) {
                            mark_lv_unmappable(lv,
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
                mark_lv_unmappable(&vg_out->lvs[a],
                                   vg_out->lv_overflow
                                       ? "volume group has too many LVs"
                                       : "volume group has too many PVs");
            }
        }
    }

    /* Validate segment contiguity, geometry, and backing PV/sub-LV integrity. */
    for (size_t a = 0; a < vg_out->lv_count; a++) {
        validate_lv_segments(vg_out, &vg_out->lvs[a]);
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
            size_t sec_checkpoint = arena->offset;
            wl_zero(vg_out, sizeof(*vg_out));
            wl_strcpy(vg_out->name, sizeof(vg_out->name), sec_name);

            int depth = 1;
            bool has_id = false;
            bool has_extent = false;
            bool has_write = false;
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
                            if (id[0] != '\0') has_id = true;
                        }
                    } else if (wl_streq(child, "system_id")) {
                        p_string(&ps, vg_out->system_id, sizeof(vg_out->system_id));
                    } else if (wl_streq(child, "status")) {
                        uint32_t st_mask = parse_status_list(&ps);
                        if (st_mask & WL_STAT_WRITE) has_write = true;
                        if (st_mask & WL_STAT_EXPORTED) vg_out->exported = 1;
                        if ((st_mask & WL_STAT_NONEMPTY) && !(st_mask & WL_STAT_RESIZEABLE)) {
                            vg_out->not_resizeable = 1;
                        }
                    } else if (wl_streq(child, "flags")) {
                        parse_tag_list(&ps, arena, &vg_out->vg_flags, &vg_out->vg_flag_count);
                    } else if (wl_streq(child, "tags")) {
                        parse_tag_list(&ps, arena, &vg_out->tags, &vg_out->tag_count);
                    } else if (wl_streq(child, "seqno")) {
                        p_int(&ps, &seqno);
                    } else if (wl_streq(child, "extent_size")) {
                        if (p_int(&ps, &extent_size)) {
                            has_extent = true;
                        }
                    } else if (wl_streq(child, "max_lv")) {
                        p_int(&ps, &vg_out->max_lv);
                    } else if (wl_streq(child, "max_pv")) {
                        p_int(&ps, &vg_out->max_pv);
                    } else if (wl_streq(child, "metadata_copies")) {
                        p_int(&ps, &vg_out->metadata_copies);
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

            if (vg_out->name[0] != '\0' && has_id && id[0] != '\0' && has_extent) {
                vg_out->pvs = (lvmcore_physical_volume *)pvs_vec.data;
                vg_out->pv_count = pvs_vec.count;
                vg_out->lvs = (lvmcore_logical_volume *)lvs_vec.data;
                vg_out->lv_count = lvs_vec.count;
                wl_strcpy(vg_out->id, sizeof(vg_out->id), id);
                vg_out->seqno = seqno;
                vg_out->extent_size_sectors = extent_size;
                vg_out->read_only = has_write ? 0 : 1;

                parse_vg_second_pass(vg_out);
                found_vg = true;
                break;
            }

            /* Non-VG top-level section: reset scratch and output before continuing. */
            arena->offset = sec_checkpoint;
            wl_zero(vg_out, sizeof(*vg_out));
        } else if (ps.pos < ps.len && ps.p[ps.pos] == '=') {
            ps.pos++;
            p_skip_value(&ps);
        } else {
            ps.pos++;
        }
    }

    if (!found_vg || vg_out->name[0] == '\0' || vg_out->id[0] == '\0') {
        wl_zero(vg_out, sizeof(*vg_out));
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

static lvmcore_status lvmcore_find_best_mda(lvmcore_byte_source *src,
                                            const lvmcore_pv_label *label,
                                            uint32_t flags,
                                            lvmcore_arena *arena,
                                            char **text_out,
                                            size_t *text_len_out,
                                            bool *is_precommitted_out,
                                            lvmcore_err *err) {
    char *best_text = NULL;
    size_t best_text_len = 0;
    bool best_is_precommitted = false;
    uint64_t best_seqno = 0;
    int have_best = 0;
    lvmcore_status last_status = LVMCORE_ERR_NO_VALID_MDA;
    if (err) wl_zero(err, sizeof(*err));

    lvmcore_volume_group *cand_vg = (lvmcore_volume_group *)lvmcore_arena_alloc_zero(
        arena, sizeof(lvmcore_volume_group), 8);
    if (!cand_vg) {
        wl_set_err(err, LVMCORE_ERR_SCRATCH_EXHAUSTED, "scratch exhausted for candidate VG");
        return LVMCORE_ERR_SCRATCH_EXHAUSTED;
    }

    for (size_t i = 0; i < label->metadata_area_count; i++) {
        size_t mda_checkpoint = arena->offset;
        lvmcore_mda_header mda;
        lvmcore_err *probe_err = have_best ? NULL : err;

        lvmcore_status st = lvmcore_read_mda_header(
            src, &label->metadata_areas[i], arena, &mda, probe_err);
        if (st != LVMCORE_OK) {
            if (st == LVMCORE_ERR_SCRATCH_EXHAUSTED) return st;
            if (!have_best) last_status = st;
            arena->offset = mda_checkpoint;
            continue;
        }

        char *cand_text = NULL;
        size_t cand_text_len = 0;
        bool cand_is_precommitted = false;
        st = lvmcore_read_metadata_text(
            src, &label->metadata_areas[i], &mda, flags, arena,
            &cand_text, &cand_text_len, &cand_is_precommitted, probe_err);
        if (st != LVMCORE_OK) {
            if (st == LVMCORE_ERR_SCRATCH_EXHAUSTED) return st;
            if (!have_best) last_status = st;
            arena->offset = mda_checkpoint;
            continue;
        }

        size_t parse_checkpoint = arena->offset;
        wl_zero(cand_vg, sizeof(*cand_vg));
        st = lvmcore_parse_vg_text(cand_text, cand_text_len, arena, cand_vg, probe_err);
        arena->offset = parse_checkpoint;
        if (st == LVMCORE_ERR_SCRATCH_EXHAUSTED) return st;

        if (st == LVMCORE_OK &&
            cand_vg->name[0] != '\0' &&
            cand_vg->id[0] != '\0' &&
            cand_vg->extent_size_sectors > 0) {
            if (!have_best || cand_vg->seqno > best_seqno) {
                best_seqno = cand_vg->seqno;
                best_text = cand_text;
                best_text_len = cand_text_len;
                best_is_precommitted = cand_is_precommitted;
                have_best = 1;
            } else {
                arena->offset = mda_checkpoint;
            }
        } else {
            if (!have_best) {
                last_status = (st != LVMCORE_OK) ? st : LVMCORE_ERR_NO_VG;
            }
            arena->offset = mda_checkpoint;
        }
    }

    if (!have_best) {
        if (!err || !err->failed) {
            wl_set_err(err, last_status, "no readable metadata area found");
        }
        return last_status;
    }

    *text_out = best_text;
    *text_len_out = best_text_len;
    *is_precommitted_out = best_is_precommitted;
    wl_set_err(err, LVMCORE_OK, NULL);
    return LVMCORE_OK;
}

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

    /* Step 2 & 3: Iterate through metadata areas, select highest seqno */
    char *text = NULL;
    size_t text_len = 0;
    bool is_precommitted = false;
    st = lvmcore_find_best_mda(src, &label, flags, arena, &text, &text_len, &is_precommitted, err);
    if (st != LVMCORE_OK) return st;

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
    if (!vg || !vg->pvs || !key) return NULL;
    for (size_t i = 0; i < vg->pv_count; i++) {
        if (wl_streq(vg->pvs[i].key, key)) return &vg->pvs[i];
    }
    return NULL;
}

const lvmcore_physical_volume *lvmcore_vg_find_pv_by_uuid(const lvmcore_volume_group *vg,
                                                          const char *uuid) {
    if (!vg || !vg->pvs || !uuid) return NULL;
    for (size_t i = 0; i < vg->pv_count; i++) {
        if (wl_streq(vg->pvs[i].id, uuid)) return &vg->pvs[i];
    }
    return NULL;
}

const lvmcore_logical_volume *lvmcore_vg_find_lv(const lvmcore_volume_group *vg,
                                                 const char *name) {
    if (!vg || !vg->lvs || !name) return NULL;
    for (size_t i = 0; i < vg->lv_count; i++) {
        if (wl_streq(vg->lvs[i].name, name)) return &vg->lvs[i];
    }
    return NULL;
}

uint64_t lvmcore_lv_extent_count(const lvmcore_logical_volume *lv) {
    if (!lv || !lv->segments) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < lv->segment_count; i++) {
        if (UINT64_MAX - total < lv->segments[i].extent_count) {
            return UINT64_MAX; /* Saturate on overflow */
        }
        total += lv->segments[i].extent_count;
    }
    return total;
}
