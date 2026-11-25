/*
 * lvmcore_thin.c — Freestanding C11 Linux dm-thin v2 on-disk block engine.
 *
 * Strictly non-recursive, zero dynamic allocations, stack frames <= 512 bytes.
 * All 4 KiB metadata block workspaces reside in caller-provided `meta_scratch`
 * (12 slots * 4096 bytes = 48 KiB):
 *   slot 0: superblock (block 0)
 *   slot 1: space-map index / btree node (sm_get_rc / sm_set_rc / sm_alloc)
 *   slot 2: space-map bitmap block
 *   slot 3: space-map overflow refcount B-tree work block
 *   slot 4: space-map split/aux block
 *   slot 5: primary B-tree traversal node
 *   slot 6: primary B-tree sibling/split node
 *   slot 7: primary B-tree parent/root node
 *   slot 8: shadow-clone temp node
 *   slot 9: device_details lookup/update node
 *   slot 10..11: reserved aux blocks
 */
#include "lvmcore/thin.h"
#include "lvmcore/crc32.h"

#if defined(__GNUC__) || defined(__clang__)
#define THIN_NOINLINE __attribute__((noinline))
#else
#define THIN_NOINLINE
#endif

#define BLK_SZ          4096u
#define BTREE_HDR_SZ    32u
#define BTREE_INTERNAL  1u
#define BTREE_LEAF      2u
#define BTREE_MAX_DEPTH 8u
#define BITMAP_HDR_SZ   16u
#define BITMAP_ENTRIES  16320u /* (4096 - 16) * 4 */
#define INDEX_HDR_SZ    16u
#define INDEX_ENTRY_SZ  16u
#define MAX_META_BITMAPS 255u  /* (4096 - 16) / 16 */

#define SLOT_SB         0u
#define SLOT_SM_IDX     1u
#define SLOT_SM_BMP     2u
#define SLOT_SM_RC      3u
#define SLOT_SM_AUX     4u
#define SLOT_BT_CUR     5u
#define SLOT_BT_SIB     6u
#define SLOT_BT_PAR     7u
#define SLOT_BT_SHAD    8u
#define SLOT_DEV_DET    9u
#define SLOT_META_IDX   10u
#define SLOT_META_BMP   11u

static uint8_t *slot_ptr(uint8_t *scratch, unsigned slot) {
    return scratch + (size_t)slot * BLK_SZ;
}

static void th_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static void th_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void th_memmove(void *dst, const void *src, size_t n) {
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

static uint32_t th_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t th_le64(const uint8_t *p) {
    return (uint64_t)th_le32(p) | ((uint64_t)th_le32(p + 4) << 32);
}

static void th_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void th_put_le64(uint8_t *p, uint64_t v) {
    th_put_le32(p, (uint32_t)(v & 0xffffffffu));
    th_put_le32(p + 4, (uint32_t)(v >> 32));
}

typedef struct {
    const lvmcore_rw_source *tmeta;
    const lvmcore_rw_source *tdata;
    uint8_t                 *scratch;
    lvmcore_thin_superblock  sb;
} thin_ctx;

/* ------------------------------------------------------------------ */
/* Metadata block I/O + CRC32c verification                           */
/* ------------------------------------------------------------------ */

THIN_NOINLINE static lvmcore_status meta_read_blk(const thin_ctx *c, uint64_t bnr,
                                                  uint32_t csum_xor, uint8_t *buf) {
    if (c->sb.metadata_nr_blocks > 0 && bnr >= c->sb.metadata_nr_blocks) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    if (bnr > UINT64_MAX / BLK_SZ) return LVMCORE_ERR_OVERFLOW;
    uint64_t off = bnr * BLK_SZ;
    if (off > c->tmeta->size || BLK_SZ > c->tmeta->size - off) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    if (c->tmeta->read(c->tmeta->impl, off, buf, BLK_SZ) != 0) {
        return LVMCORE_ERR_IO;
    }
    uint32_t stored = th_le32(buf);
    uint32_t calc = lvmcore_crc32c_dm(buf + 4, BLK_SZ - 4, csum_xor);
    if (stored != calc) return LVMCORE_ERR_CORRUPT_METADATA;
    if (th_le64(buf + 8) != bnr) return LVMCORE_ERR_CORRUPT_METADATA;
    return LVMCORE_OK;
}

THIN_NOINLINE static lvmcore_status meta_write_blk(const thin_ctx *c, uint64_t bnr,
                                                   uint32_t csum_xor, uint8_t *buf) {
    if (!c->tmeta->write) return LVMCORE_ERR_IO;
    if (c->sb.metadata_nr_blocks > 0 && bnr >= c->sb.metadata_nr_blocks) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    if (bnr > UINT64_MAX / BLK_SZ) return LVMCORE_ERR_OVERFLOW;
    uint64_t off = bnr * BLK_SZ;
    if (off > c->tmeta->size || BLK_SZ > c->tmeta->size - off) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    th_put_le64(buf + 8, bnr);
    uint32_t crc = lvmcore_crc32c_dm(buf + 4, BLK_SZ - 4, csum_xor);
    th_put_le32(buf, crc);
    return (c->tmeta->write(c->tmeta->impl, off, buf, BLK_SZ) == 0)
               ? LVMCORE_OK
               : LVMCORE_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* Superblock decode / encode                                         */
/* ------------------------------------------------------------------ */

static void decode_sm_root(const uint8_t *p, lvmcore_thin_sm_root *r) {
    r->nr_blocks = th_le64(p);
    r->nr_allocated = th_le64(p + 8);
    r->bitmap_root = th_le64(p + 16);
    r->ref_count_root = th_le64(p + 24);
}

static void encode_sm_root(uint8_t *p, const lvmcore_thin_sm_root *r) {
    th_put_le64(p, r->nr_blocks);
    th_put_le64(p + 8, r->nr_allocated);
    th_put_le64(p + 16, r->bitmap_root);
    th_put_le64(p + 24, r->ref_count_root);
}

THIN_NOINLINE lvmcore_status lvmcore_thin_read_superblock(const lvmcore_rw_source *tmeta,
                                                          uint8_t *meta_buf,
                                                          lvmcore_thin_superblock *sb_out) {
    if (!tmeta || !tmeta->read || !meta_buf || !sb_out) return LVMCORE_ERR_INVALID_ARG;
    th_zero(sb_out, sizeof(*sb_out));
    if (tmeta->size < BLK_SZ) return LVMCORE_ERR_CORRUPT_METADATA;

    if (tmeta->read(tmeta->impl, 0, meta_buf, BLK_SZ) != 0) return LVMCORE_ERR_IO;
    uint32_t stored = th_le32(meta_buf);
    uint32_t calc = lvmcore_crc32c_dm(meta_buf + 4, BLK_SZ - 4, LVMCORE_THIN_SUPERBLOCK_CSUM_XOR);
    if (stored != calc) return LVMCORE_ERR_CORRUPT_METADATA;

    sb_out->flags = th_le32(meta_buf + 4);
    sb_out->blocknr = th_le64(meta_buf + 8);
    sb_out->magic = th_le64(meta_buf + 32);
    sb_out->version = th_le32(meta_buf + 40);
    sb_out->time = th_le32(meta_buf + 44);
    sb_out->trans_id = th_le64(meta_buf + 48);
    sb_out->metadata_snap = th_le64(meta_buf + 56);
    decode_sm_root(meta_buf + 64, &sb_out->data_sm);
    decode_sm_root(meta_buf + 192, &sb_out->meta_sm);
    sb_out->data_mapping_root = th_le64(meta_buf + 320);
    sb_out->device_details_root = th_le64(meta_buf + 328);
    sb_out->data_block_size = th_le32(meta_buf + 336);
    sb_out->metadata_block_size = th_le32(meta_buf + 340);
    sb_out->metadata_nr_blocks = th_le64(meta_buf + 344);

    if (sb_out->blocknr != 0 || sb_out->magic != LVMCORE_THIN_SUPERBLOCK_MAGIC) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    if ((sb_out->flags & 0x1u) != 0) {
        /* THIN_METADATA_NEEDS_CHECK_FLAG set: refuse mounting dirty metadata. */
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    if (sb_out->version != LVMCORE_THIN_VERSION || sb_out->metadata_block_size != 8u ||
        sb_out->data_block_size == 0 || sb_out->data_block_size > 2097152u ||
        sb_out->metadata_snap != 0) {
        return LVMCORE_ERR_UNSUPPORTED;
    }
    if (sb_out->metadata_nr_blocks == 0 || sb_out->metadata_nr_blocks > (tmeta->size / BLK_SZ)) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    if (sb_out->meta_sm.nr_blocks == 0 || sb_out->meta_sm.nr_blocks > sb_out->metadata_nr_blocks) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    uint64_t chunk_bytes = (uint64_t)sb_out->data_block_size * 512ULL;
    if (chunk_bytes == 0 || sb_out->data_sm.nr_blocks > UINT64_MAX / chunk_bytes) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    return LVMCORE_OK;
}

THIN_NOINLINE static lvmcore_status flush_superblock(thin_ctx *c) {
    uint8_t *sb_buf = slot_ptr(c->scratch, SLOT_SB);
    th_put_le32(sb_buf + 4, c->sb.flags);
    th_put_le64(sb_buf + 8, 0ULL);
    th_put_le64(sb_buf + 32, c->sb.magic);
    th_put_le32(sb_buf + 40, c->sb.version);
    th_put_le32(sb_buf + 44, c->sb.time);
    th_put_le64(sb_buf + 48, c->sb.trans_id);
    th_put_le64(sb_buf + 56, c->sb.metadata_snap);
    encode_sm_root(sb_buf + 64, &c->sb.data_sm);
    encode_sm_root(sb_buf + 192, &c->sb.meta_sm);
    th_put_le64(sb_buf + 320, c->sb.data_mapping_root);
    th_put_le64(sb_buf + 328, c->sb.device_details_root);
    th_put_le32(sb_buf + 336, c->sb.data_block_size);
    th_put_le32(sb_buf + 340, c->sb.metadata_block_size);
    th_put_le64(sb_buf + 344, c->sb.metadata_nr_blocks);
    return meta_write_blk(c, 0ULL, LVMCORE_THIN_SUPERBLOCK_CSUM_XOR, sb_buf);
}

/* ------------------------------------------------------------------ */
/* B-tree helpers (iterative, non-recursive, stack <= 512B)           */
/* ------------------------------------------------------------------ */

static uint32_t btree_max_entries_for(uint32_t vsz) {
    uint32_t raw = (BLK_SZ - BTREE_HDR_SZ) / (8u + vsz);
    return (raw / 3u) * 3u;
}

static uint64_t btree_key(const uint8_t *node, uint32_t idx) {
    return th_le64(node + BTREE_HDR_SZ + (size_t)idx * 8u);
}

static void btree_set_key(uint8_t *node, uint32_t idx, uint64_t k) {
    th_put_le64(node + BTREE_HDR_SZ + (size_t)idx * 8u, k);
}

static uint8_t *btree_val_ptr(uint8_t *node, uint32_t max_ent, uint32_t vsz, uint32_t idx) {
    return node + BTREE_HDR_SZ + (size_t)max_ent * 8u + (size_t)idx * vsz;
}

static const uint8_t *btree_val_cptr(const uint8_t *node, uint32_t max_ent,
                                     uint32_t vsz, uint32_t idx) {
    return node + BTREE_HDR_SZ + (size_t)max_ent * 8u + (size_t)idx * vsz;
}

static void btree_init_node(uint8_t *node, uint32_t flags, uint64_t bnr, uint32_t vsz) {
    th_zero(node, BLK_SZ);
    th_put_le32(node + 4, flags);
    th_put_le64(node + 8, bnr);
    th_put_le32(node + 16, 0u);
    th_put_le32(node + 20, btree_max_entries_for(vsz));
    th_put_le32(node + 24, vsz);
}

static int btree_validate_node(const uint8_t *node, uint32_t expected_leaf_vsz) {
    uint32_t fl = th_le32(node + 4);
    uint32_t nr = th_le32(node + 16);
    uint32_t mx = th_le32(node + 20);
    uint32_t vsz = th_le32(node + 24);
    if (fl != BTREE_INTERNAL && fl != BTREE_LEAF) return 0;
    if (nr > mx || mx == 0) return 0;
    uint32_t want_vsz = (fl == BTREE_INTERNAL) ? 8u : expected_leaf_vsz;
    if (vsz != want_vsz) return 0;
    if ((size_t)BTREE_HDR_SZ + (size_t)mx * (8u + (size_t)vsz) > BLK_SZ) return 0;
    return 1;
}

/* Binary search returning index of first key >= target, or nr if all < target. */
static uint32_t btree_lower_bound(const uint8_t *node, uint32_t nr, uint64_t target) {
    uint32_t lo = 0, hi = nr;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (btree_key(node, mid) < target) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

/*
 * Iterative B-tree lookup. Returns:
 *   1 if found (value copied to val_out),
 *   0 if not found,
 *  -1 on error (status written to *st_out).
 */
THIN_NOINLINE static int btree_lookup(const thin_ctx *c, uint64_t root, uint64_t key,
                                      uint32_t vsz, unsigned work_slot,
                                      void *val_out, lvmcore_status *st_out) {
    uint8_t *node = slot_ptr(c->scratch, work_slot);
    uint64_t cur = root;
    *st_out = LVMCORE_OK;

    for (unsigned depth = 0; depth < BTREE_MAX_DEPTH; depth++) {
        lvmcore_status st = meta_read_blk(c, cur, LVMCORE_THIN_BTREE_CSUM_XOR, node);
        if (st != LVMCORE_OK) { *st_out = st; return -1; }
        if (!btree_validate_node(node, vsz)) {
            *st_out = LVMCORE_ERR_CORRUPT_METADATA;
            return -1;
        }
        uint32_t fl = th_le32(node + 4);
        uint32_t nr = th_le32(node + 16);
        uint32_t mx = th_le32(node + 20);
        if (fl == BTREE_LEAF) {
            uint32_t pos = btree_lower_bound(node, nr, key);
            if (pos < nr && btree_key(node, pos) == key) {
                if (val_out) th_memcpy(val_out, btree_val_cptr(node, mx, vsz, pos), vsz);
                return 1;
            }
            return 0;
        }
        /* INTERNAL node: choose largest child key <= target (or child 0 if target < keys[0]). */
        if (nr == 0) return 0;
        uint32_t pos = btree_lower_bound(node, nr, key);
        uint32_t child_idx = 0;
        if (pos < nr && btree_key(node, pos) == key) child_idx = pos;
        else if (pos > 0) child_idx = pos - 1u;
        else child_idx = 0;
        cur = th_le64(btree_val_cptr(node, mx, 8u, child_idx));
    }
    *st_out = LVMCORE_ERR_CORRUPT_METADATA;
    return -1;
}

/* Forward declarations for space-map & B-tree mutual operations. */
static lvmcore_status sm_alloc_meta(thin_ctx *c, uint64_t *blk_out);
static lvmcore_status sm_get_rc(thin_ctx *c, int is_meta, uint64_t blk, uint32_t *rc_out);
static lvmcore_status sm_adjust_rc(thin_ctx *c, int is_meta, uint64_t blk, int delta);

/* Insert or update (key -> val) in a simple unshared B-tree (no shadowing). */
THIN_NOINLINE static lvmcore_status btree_upsert_simple(thin_ctx *c, uint64_t *root_ptr,
                                                        uint64_t key, const void *val,
                                                        uint32_t vsz,
                                                        unsigned cur_slot, unsigned sib_slot,
                                                        unsigned par_slot,
                                                        int *inserted_out) {
    uint64_t path_blk[BTREE_MAX_DEPTH];
    uint32_t path_idx[BTREE_MAX_DEPTH];
    unsigned depth = 0;
    uint64_t cur = *root_ptr;
    uint8_t *node = slot_ptr(c->scratch, cur_slot);
    if (inserted_out) *inserted_out = 0;

    for (;;) {
        if (depth >= BTREE_MAX_DEPTH) return LVMCORE_ERR_CORRUPT_METADATA;
        lvmcore_status st = meta_read_blk(c, cur, LVMCORE_THIN_BTREE_CSUM_XOR, node);
        if (st != LVMCORE_OK) return st;
        if (!btree_validate_node(node, vsz)) return LVMCORE_ERR_CORRUPT_METADATA;

        path_blk[depth] = cur;
        uint32_t fl = th_le32(node + 4);
        uint32_t nr = th_le32(node + 16);
        uint32_t mx = th_le32(node + 20);
        if (fl == BTREE_LEAF) break;
        if (nr == 0) return LVMCORE_ERR_CORRUPT_METADATA;

        uint32_t pos = btree_lower_bound(node, nr, key);
        uint32_t child_idx = 0;
        if (pos < nr && btree_key(node, pos) == key) child_idx = pos;
        else if (pos > 0) child_idx = pos - 1u;
        path_idx[depth] = child_idx;
        cur = th_le64(btree_val_cptr(node, mx, 8u, child_idx));
        depth++;
    }

    /* `node` holds leaf at `path_blk[depth]`. */
    uint32_t nr = th_le32(node + 16);
    uint32_t mx = th_le32(node + 20);
    uint32_t pos = btree_lower_bound(node, nr, key);

    if (pos < nr && btree_key(node, pos) == key) {
        th_memcpy(btree_val_ptr(node, mx, vsz, pos), val, vsz);
        return meta_write_blk(c, path_blk[depth], LVMCORE_THIN_BTREE_CSUM_XOR, node);
    }

    if (inserted_out) *inserted_out = 1;

    if (nr < mx) {
        if (pos < nr) {
            th_memmove(node + BTREE_HDR_SZ + (size_t)(pos + 1u) * 8u,
                       node + BTREE_HDR_SZ + (size_t)pos * 8u,
                       (size_t)(nr - pos) * 8u);
            th_memmove(btree_val_ptr(node, mx, vsz, pos + 1u),
                       btree_val_ptr(node, mx, vsz, pos),
                       (size_t)(nr - pos) * vsz);
        }
        btree_set_key(node, pos, key);
        th_memcpy(btree_val_ptr(node, mx, vsz, pos), val, vsz);
        th_put_le32(node + 16, nr + 1u);
        lvmcore_status st = meta_write_blk(c, path_blk[depth], LVMCORE_THIN_BTREE_CSUM_XOR, node);
        if (st != LVMCORE_OK) return st;

        /* If minimum key of this leaf decreased, update parent internal node keys upward. */
        if (pos == 0 && depth > 0) {
            uint64_t new_min = key;
            for (unsigned d = depth; d > 0; d--) {
                uint8_t *par = slot_ptr(c->scratch, par_slot);
                st = meta_read_blk(c, path_blk[d - 1u], LVMCORE_THIN_BTREE_CSUM_XOR, par);
                if (st != LVMCORE_OK) return st;
                btree_set_key(par, path_idx[d - 1u], new_min);
                st = meta_write_blk(c, path_blk[d - 1u], LVMCORE_THIN_BTREE_CSUM_XOR, par);
                if (st != LVMCORE_OK) return st;
                if (path_idx[d - 1u] != 0) break;
            }
        }
        return LVMCORE_OK;
    }

    /* Leaf is full: split into left (existing block) and right (new block). */
    if (mx < 2u || nr < 2u) return LVMCORE_ERR_CORRUPT_METADATA;
    uint64_t right_blk = 0;
    lvmcore_status st = sm_alloc_meta(c, &right_blk);
    if (st != LVMCORE_OK) return st;

    uint8_t *right = slot_ptr(c->scratch, sib_slot);
    btree_init_node(right, BTREE_LEAF, right_blk, vsz);
    uint32_t rmx = th_le32(right + 20);

    uint32_t mid = nr / 2u;
    uint32_t right_nr = nr - mid;
    if (mid == 0u || right_nr == 0u || right_nr + 1u > rmx) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    th_memcpy(right + BTREE_HDR_SZ, node + BTREE_HDR_SZ + (size_t)mid * 8u, (size_t)right_nr * 8u);
    th_memcpy(btree_val_ptr(right, rmx, vsz, 0), btree_val_ptr(node, mx, vsz, mid),
              (size_t)right_nr * vsz);
    th_put_le32(node + 16, mid);
    th_put_le32(right + 16, right_nr);

    if (pos <= mid) {
        if (pos < mid) {
            th_memmove(node + BTREE_HDR_SZ + (size_t)(pos + 1u) * 8u,
                       node + BTREE_HDR_SZ + (size_t)pos * 8u,
                       (size_t)(mid - pos) * 8u);
            th_memmove(btree_val_ptr(node, mx, vsz, pos + 1u),
                       btree_val_ptr(node, mx, vsz, pos),
                       (size_t)(mid - pos) * vsz);
        }
        btree_set_key(node, pos, key);
        th_memcpy(btree_val_ptr(node, mx, vsz, pos), val, vsz);
        th_put_le32(node + 16, mid + 1u);
    } else {
        uint32_t rpos = pos - mid;
        if (rpos < right_nr) {
            th_memmove(right + BTREE_HDR_SZ + (size_t)(rpos + 1u) * 8u,
                       right + BTREE_HDR_SZ + (size_t)rpos * 8u,
                       (size_t)(right_nr - rpos) * 8u);
            th_memmove(btree_val_ptr(right, rmx, vsz, rpos + 1u),
                       btree_val_ptr(right, rmx, vsz, rpos),
                       (size_t)(right_nr - rpos) * vsz);
        }
        btree_set_key(right, rpos, key);
        th_memcpy(btree_val_ptr(right, rmx, vsz, rpos), val, vsz);
        th_put_le32(right + 16, right_nr + 1u);
    }

    uint64_t left_min = btree_key(node, 0);
    uint64_t promo_key = btree_key(right, 0);
    uint64_t promo_blk = right_blk;

    st = meta_write_blk(c, path_blk[depth], LVMCORE_THIN_BTREE_CSUM_XOR, node);
    if (st != LVMCORE_OK) return st;
    st = meta_write_blk(c, right_blk, LVMCORE_THIN_BTREE_CSUM_XOR, right);
    if (st != LVMCORE_OK) return st;

    /* Propagate promoted separator upward iteratively. */
    uint64_t left_blk = path_blk[depth];
    for (;;) {
        if (depth == 0) {
            uint64_t new_root = 0;
            st = sm_alloc_meta(c, &new_root);
            if (st != LVMCORE_OK) return st;
            uint8_t *rnode = slot_ptr(c->scratch, par_slot);
            btree_init_node(rnode, BTREE_INTERNAL, new_root, 8u);
            uint32_t root_mx = th_le32(rnode + 20);
            btree_set_key(rnode, 0, left_min);
            th_put_le64(btree_val_ptr(rnode, root_mx, 8u, 0), left_blk);
            btree_set_key(rnode, 1, promo_key);
            th_put_le64(btree_val_ptr(rnode, root_mx, 8u, 1), promo_blk);
            th_put_le32(rnode + 16, 2u);
            st = meta_write_blk(c, new_root, LVMCORE_THIN_BTREE_CSUM_XOR, rnode);
            if (st != LVMCORE_OK) return st;
            *root_ptr = new_root;
            return LVMCORE_OK;
        }

        depth--;
        uint8_t *par = slot_ptr(c->scratch, cur_slot);
        st = meta_read_blk(c, path_blk[depth], LVMCORE_THIN_BTREE_CSUM_XOR, par);
        if (st != LVMCORE_OK) return st;
        uint32_t pnr = th_le32(par + 16);
        uint32_t pmx = th_le32(par + 20);
        uint32_t cidx = path_idx[depth];
        btree_set_key(par, cidx, left_min);
        uint32_t ins = cidx + 1u;

        if (pnr < pmx) {
            if (ins < pnr) {
                th_memmove(par + BTREE_HDR_SZ + (size_t)(ins + 1u) * 8u,
                           par + BTREE_HDR_SZ + (size_t)ins * 8u,
                           (size_t)(pnr - ins) * 8u);
                th_memmove(btree_val_ptr(par, pmx, 8u, ins + 1u),
                           btree_val_ptr(par, pmx, 8u, ins),
                           (size_t)(pnr - ins) * 8u);
            }
            btree_set_key(par, ins, promo_key);
            th_put_le64(btree_val_ptr(par, pmx, 8u, ins), promo_blk);
            th_put_le32(par + 16, pnr + 1u);
            st = meta_write_blk(c, path_blk[depth], LVMCORE_THIN_BTREE_CSUM_XOR, par);
            if (st != LVMCORE_OK) return st;
            if (cidx == 0 && depth > 0) {
                uint64_t up_min = btree_key(par, 0);
                for (unsigned d = depth; d > 0; d--) {
                    uint8_t *up = slot_ptr(c->scratch, par_slot);
                    st = meta_read_blk(c, path_blk[d - 1u], LVMCORE_THIN_BTREE_CSUM_XOR, up);
                    if (st != LVMCORE_OK) return st;
                    btree_set_key(up, path_idx[d - 1u], up_min);
                    st = meta_write_blk(c, path_blk[d - 1u], LVMCORE_THIN_BTREE_CSUM_XOR, up);
                    if (st != LVMCORE_OK) return st;
                    if (path_idx[d - 1u] != 0) break;
                }
            }
            return LVMCORE_OK;
        }

        /* Internal node also full: split it and keep climbing. */
        if (pmx < 2u || pnr < 2u) return LVMCORE_ERR_CORRUPT_METADATA;
        uint64_t pright_blk = 0;
        st = sm_alloc_meta(c, &pright_blk);
        if (st != LVMCORE_OK) return st;
        uint8_t *pright = slot_ptr(c->scratch, sib_slot);
        btree_init_node(pright, BTREE_INTERNAL, pright_blk, 8u);
        uint32_t prmx = th_le32(pright + 20);

        uint32_t pmid = pnr / 2u;
        uint32_t prnr = pnr - pmid;
        if (pmid == 0u || prnr == 0u || prnr + 1u > prmx) {
            return LVMCORE_ERR_CORRUPT_METADATA;
        }
        th_memcpy(pright + BTREE_HDR_SZ, par + BTREE_HDR_SZ + (size_t)pmid * 8u, (size_t)prnr * 8u);
        th_memcpy(btree_val_ptr(pright, prmx, 8u, 0), btree_val_ptr(par, pmx, 8u, pmid),
                  (size_t)prnr * 8u);
        th_put_le32(par + 16, pmid);
        th_put_le32(pright + 16, prnr);

        if (ins <= pmid) {
            if (ins < pmid) {
                th_memmove(par + BTREE_HDR_SZ + (size_t)(ins + 1u) * 8u,
                           par + BTREE_HDR_SZ + (size_t)ins * 8u,
                           (size_t)(pmid - ins) * 8u);
                th_memmove(btree_val_ptr(par, pmx, 8u, ins + 1u),
                           btree_val_ptr(par, pmx, 8u, ins),
                           (size_t)(pmid - ins) * 8u);
            }
            btree_set_key(par, ins, promo_key);
            th_put_le64(btree_val_ptr(par, pmx, 8u, ins), promo_blk);
            th_put_le32(par + 16, pmid + 1u);
        } else {
            uint32_t rins = ins - pmid;
            if (rins < prnr) {
                th_memmove(pright + BTREE_HDR_SZ + (size_t)(rins + 1u) * 8u,
                           pright + BTREE_HDR_SZ + (size_t)rins * 8u,
                           (size_t)(prnr - rins) * 8u);
                th_memmove(btree_val_ptr(pright, prmx, 8u, rins + 1u),
                           btree_val_ptr(pright, prmx, 8u, rins),
                           (size_t)(prnr - rins) * 8u);
            }
            btree_set_key(pright, rins, promo_key);
            th_put_le64(btree_val_ptr(pright, prmx, 8u, rins), promo_blk);
            th_put_le32(pright + 16, prnr + 1u);
        }

        left_min = btree_key(par, 0);
        promo_key = btree_key(pright, 0);
        promo_blk = pright_blk;
        left_blk = path_blk[depth];
        st = meta_write_blk(c, left_blk, LVMCORE_THIN_BTREE_CSUM_XOR, par);
        if (st != LVMCORE_OK) return st;
        st = meta_write_blk(c, pright_blk, LVMCORE_THIN_BTREE_CSUM_XOR, pright);
        if (st != LVMCORE_OK) return st;
    }
}

/* Remove a key from a simple unshared leaf (used when overflow refcount drops from 3 -> 2). */
THIN_NOINLINE static lvmcore_status btree_remove_simple(thin_ctx *c, uint64_t root,
                                                        uint64_t key, uint32_t vsz,
                                                        unsigned work_slot) {
    uint8_t *node = slot_ptr(c->scratch, work_slot);
    uint64_t cur = root;
    for (unsigned depth = 0; depth < BTREE_MAX_DEPTH; depth++) {
        lvmcore_status st = meta_read_blk(c, cur, LVMCORE_THIN_BTREE_CSUM_XOR, node);
        if (st != LVMCORE_OK) return st;
        if (!btree_validate_node(node, vsz)) return LVMCORE_ERR_CORRUPT_METADATA;
        uint32_t fl = th_le32(node + 4);
        uint32_t nr = th_le32(node + 16);
        uint32_t mx = th_le32(node + 20);
        if (fl == BTREE_LEAF) {
            uint32_t pos = btree_lower_bound(node, nr, key);
            if (pos >= nr || btree_key(node, pos) != key) return LVMCORE_OK;
            if (pos + 1u < nr) {
                th_memmove(node + BTREE_HDR_SZ + (size_t)pos * 8u,
                           node + BTREE_HDR_SZ + (size_t)(pos + 1u) * 8u,
                           (size_t)(nr - pos - 1u) * 8u);
                th_memmove(btree_val_ptr(node, mx, vsz, pos),
                           btree_val_ptr(node, mx, vsz, pos + 1u),
                           (size_t)(nr - pos - 1u) * vsz);
            }
            th_put_le32(node + 16, nr - 1u);
            return meta_write_blk(c, cur, LVMCORE_THIN_BTREE_CSUM_XOR, node);
        }
        if (nr == 0) return LVMCORE_OK;
        uint32_t pos = btree_lower_bound(node, nr, key);
        uint32_t child_idx = (pos < nr && btree_key(node, pos) == key) ? pos : (pos > 0 ? pos - 1u : 0u);
        cur = th_le64(btree_val_cptr(node, mx, 8u, child_idx));
    }
    return LVMCORE_ERR_CORRUPT_METADATA;
}

/* ------------------------------------------------------------------ */
/* Space-map operations (2-bit bit-swapped dm-space-map-common)       */
/* ------------------------------------------------------------------ */

static uint32_t bmp_read_2bit(const uint8_t *bmp, uint32_t entry_idx) {
    uint32_t byte = (uint32_t)bmp[BITMAP_HDR_SZ + (entry_idx >> 2)];
    uint32_t raw = (byte >> ((entry_idx & 3u) * 2u)) & 3u;
    /* Linux dm-space-map bit-swapped encoding: 0->0, 2->1, 1->2, 3->3+ */
    if (raw == 0u) return 0u;
    if (raw == 2u) return 1u;
    if (raw == 1u) return 2u;
    return 3u;
}

static void bmp_write_2bit(uint8_t *bmp, uint32_t entry_idx, uint32_t rc_clamped) {
    uint32_t raw = 0u;
    if (rc_clamped == 1u) raw = 2u;
    else if (rc_clamped == 2u) raw = 1u;
    else if (rc_clamped >= 3u) raw = 3u;
    uint32_t shift = (entry_idx & 3u) * 2u;
    uint8_t *byte = &bmp[BITMAP_HDR_SZ + (entry_idx >> 2)];
    *byte = (uint8_t)((((uint32_t)(*byte)) & ~(3u << shift)) | (raw << shift));
}

typedef struct {
    uint64_t blocknr;
    uint32_t nr_free;
    uint32_t none_free_before;
} sm_index_entry;

THIN_NOINLINE static lvmcore_status sm_read_index_entry(const thin_ctx *c, int is_meta,
                                                        uint64_t idx_i,
                                                        sm_index_entry *ent_out) {
    const lvmcore_thin_sm_root *sm = is_meta ? &c->sb.meta_sm : &c->sb.data_sm;
    if (is_meta) {
        if (idx_i >= MAX_META_BITMAPS) return LVMCORE_ERR_CORRUPT_METADATA;
        uint8_t *idx_blk = slot_ptr(c->scratch, SLOT_META_IDX);
        lvmcore_status st = meta_read_blk(c, sm->bitmap_root, LVMCORE_THIN_INDEX_CSUM_XOR, idx_blk);
        if (st != LVMCORE_OK) return st;
        const uint8_t *p = idx_blk + INDEX_HDR_SZ + (size_t)idx_i * INDEX_ENTRY_SZ;
        ent_out->blocknr = th_le64(p);
        ent_out->nr_free = th_le32(p + 8);
        ent_out->none_free_before = th_le32(p + 12);
        return LVMCORE_OK;
    } else {
        uint8_t raw[16];
        lvmcore_status st = LVMCORE_OK;
        int found = btree_lookup(c, sm->bitmap_root, idx_i, 16u, SLOT_SM_IDX, raw, &st);
        if (found < 0) return st;
        if (found == 0) return LVMCORE_ERR_CORRUPT_METADATA;
        ent_out->blocknr = th_le64(raw);
        ent_out->nr_free = th_le32(raw + 8);
        ent_out->none_free_before = th_le32(raw + 12);
        return LVMCORE_OK;
    }
}

THIN_NOINLINE static lvmcore_status sm_write_index_entry(thin_ctx *c, int is_meta,
                                                         uint64_t idx_i,
                                                         const sm_index_entry *ent) {
    lvmcore_thin_sm_root *sm = is_meta ? &c->sb.meta_sm : &c->sb.data_sm;
    if (is_meta) {
        if (idx_i >= MAX_META_BITMAPS) return LVMCORE_ERR_CORRUPT_METADATA;
        uint8_t *idx_blk = slot_ptr(c->scratch, SLOT_META_IDX);
        lvmcore_status st = meta_read_blk(c, sm->bitmap_root, LVMCORE_THIN_INDEX_CSUM_XOR, idx_blk);
        if (st != LVMCORE_OK) return st;
        uint8_t *p = idx_blk + INDEX_HDR_SZ + (size_t)idx_i * INDEX_ENTRY_SZ;
        th_put_le64(p, ent->blocknr);
        th_put_le32(p + 8, ent->nr_free);
        th_put_le32(p + 12, ent->none_free_before);
        return meta_write_blk(c, sm->bitmap_root, LVMCORE_THIN_INDEX_CSUM_XOR, idx_blk);
    } else {
        uint8_t raw[16];
        th_put_le64(raw, ent->blocknr);
        th_put_le32(raw + 8, ent->nr_free);
        th_put_le32(raw + 12, ent->none_free_before);
        return btree_upsert_simple(c, &sm->bitmap_root, idx_i, raw, 16u,
                                   SLOT_SM_IDX, SLOT_SM_AUX, SLOT_SM_RC, NULL);
    }
}

THIN_NOINLINE static lvmcore_status sm_get_rc(thin_ctx *c, int is_meta,
                                              uint64_t blk, uint32_t *rc_out) {
    const lvmcore_thin_sm_root *sm = is_meta ? &c->sb.meta_sm : &c->sb.data_sm;
    if (blk >= sm->nr_blocks) return LVMCORE_ERR_CORRUPT_METADATA;

    uint64_t idx_i = blk / BITMAP_ENTRIES;
    uint32_t bit_i = (uint32_t)(blk % BITMAP_ENTRIES);
    sm_index_entry ent;
    lvmcore_status st = sm_read_index_entry(c, is_meta, idx_i, &ent);
    if (st != LVMCORE_OK) return st;

    uint8_t *bmp = slot_ptr(c->scratch, is_meta ? SLOT_META_BMP : SLOT_SM_BMP);
    st = meta_read_blk(c, ent.blocknr, LVMCORE_THIN_BITMAP_CSUM_XOR, bmp);
    if (st != LVMCORE_OK) return st;

    uint32_t small = bmp_read_2bit(bmp, bit_i);
    if (small < 3u) {
        *rc_out = small;
        return LVMCORE_OK;
    }

    uint8_t rc_bytes[4];
    int found = btree_lookup(c, sm->ref_count_root, blk, 4u, SLOT_SM_RC, rc_bytes, &st);
    if (found <= 0) return (found < 0) ? st : LVMCORE_ERR_CORRUPT_METADATA;
    *rc_out = th_le32(rc_bytes);
    return LVMCORE_OK;
}

THIN_NOINLINE static lvmcore_status sm_adjust_rc(thin_ctx *c, int is_meta,
                                                 uint64_t blk, int delta) {
    lvmcore_thin_sm_root *sm = is_meta ? &c->sb.meta_sm : &c->sb.data_sm;
    if (blk >= sm->nr_blocks) return LVMCORE_ERR_CORRUPT_METADATA;

    uint32_t old_rc = 0;
    lvmcore_status st = sm_get_rc(c, is_meta, blk, &old_rc);
    if (st != LVMCORE_OK) return st;

    if (delta < 0 && old_rc < (uint32_t)(-delta)) return LVMCORE_ERR_CORRUPT_METADATA;
    if (delta > 0 && old_rc > UINT32_MAX - (uint32_t)delta) return LVMCORE_ERR_OVERFLOW;
    uint32_t new_rc = (delta >= 0) ? (old_rc + (uint32_t)delta) : (old_rc - (uint32_t)(-delta));
    if (new_rc == old_rc) return LVMCORE_OK;

    uint64_t idx_i = blk / BITMAP_ENTRIES;
    uint32_t bit_i = (uint32_t)(blk % BITMAP_ENTRIES);
    sm_index_entry ent;
    st = sm_read_index_entry(c, is_meta, idx_i, &ent);
    if (st != LVMCORE_OK) return st;

    uint8_t *bmp = slot_ptr(c->scratch, is_meta ? SLOT_META_BMP : SLOT_SM_BMP);
    st = meta_read_blk(c, ent.blocknr, LVMCORE_THIN_BITMAP_CSUM_XOR, bmp);
    if (st != LVMCORE_OK) return st;

    bmp_write_2bit(bmp, bit_i, (new_rc >= 3u) ? 3u : new_rc);
    st = meta_write_blk(c, ent.blocknr, LVMCORE_THIN_BITMAP_CSUM_XOR, bmp);
    if (st != LVMCORE_OK) return st;

    if (old_rc == 0 && new_rc > 0) {
        if (ent.nr_free > 0) ent.nr_free--;
        sm->nr_allocated++;
    } else if (old_rc > 0 && new_rc == 0) {
        ent.nr_free++;
        if (bit_i < ent.none_free_before) ent.none_free_before = bit_i;
        if (sm->nr_allocated > 0) sm->nr_allocated--;
    }

    st = sm_write_index_entry(c, is_meta, idx_i, &ent);
    if (st != LVMCORE_OK) return st;

    if (new_rc >= 3u) {
        uint8_t rc_bytes[4];
        th_put_le32(rc_bytes, new_rc);
        st = btree_upsert_simple(c, &sm->ref_count_root, blk, rc_bytes, 4u,
                                 SLOT_SM_RC, SLOT_SM_AUX, SLOT_SM_IDX, NULL);
        if (st != LVMCORE_OK) return st;
    } else if (old_rc >= 3u) {
        st = btree_remove_simple(c, sm->ref_count_root, blk, 4u, SLOT_SM_RC);
        if (st != LVMCORE_OK) return st;
    }
    return LVMCORE_OK;
}

THIN_NOINLINE static lvmcore_status sm_alloc_block(thin_ctx *c, int is_meta, uint64_t *blk_out) {
    lvmcore_thin_sm_root *sm = is_meta ? &c->sb.meta_sm : &c->sb.data_sm;
    if (sm->nr_blocks == 0 || sm->nr_allocated >= sm->nr_blocks) return LVMCORE_ERR_IO;

    uint64_t nr_bitmaps = ((sm->nr_blocks - 1ULL) / BITMAP_ENTRIES) + 1ULL;
    for (uint64_t idx_i = 0; idx_i < nr_bitmaps; idx_i++) {
        sm_index_entry ent;
        lvmcore_status st = sm_read_index_entry(c, is_meta, idx_i, &ent);
        if (st != LVMCORE_OK) return st;
        if (ent.nr_free == 0) continue;

        uint64_t base_blk = idx_i * BITMAP_ENTRIES;
        uint64_t limit = sm->nr_blocks - base_blk;
        if (limit > BITMAP_ENTRIES) limit = BITMAP_ENTRIES;

        uint8_t *bmp = slot_ptr(c->scratch, is_meta ? SLOT_META_BMP : SLOT_SM_BMP);
        st = meta_read_blk(c, ent.blocknr, LVMCORE_THIN_BITMAP_CSUM_XOR, bmp);
        if (st != LVMCORE_OK) return st;

        uint32_t start_bit = (ent.none_free_before < limit) ? ent.none_free_before : 0u;
        for (uint32_t pass = 0; pass < 2u; pass++) {
            uint32_t lo = (pass == 0) ? start_bit : 0u;
            uint32_t hi = (pass == 0) ? (uint32_t)limit : start_bit;
            for (uint32_t bit = lo; bit < hi; bit++) {
                if (bmp_read_2bit(bmp, bit) == 0u) {
                    bmp_write_2bit(bmp, bit, 1u);
                    st = meta_write_blk(c, ent.blocknr, LVMCORE_THIN_BITMAP_CSUM_XOR, bmp);
                    if (st != LVMCORE_OK) return st;

                    ent.nr_free--;
                    ent.none_free_before = bit + 1u;
                    sm->nr_allocated++;
                    st = sm_write_index_entry(c, is_meta, idx_i, &ent);
                    if (st != LVMCORE_OK) return st;

                    *blk_out = base_blk + (uint64_t)bit;
                    return LVMCORE_OK;
                }
            }
        }
    }
    return LVMCORE_ERR_IO;
}

static lvmcore_status sm_alloc_meta(thin_ctx *c, uint64_t *blk_out) {
    return sm_alloc_block(c, 1, blk_out);
}

/* ------------------------------------------------------------------ */
/* Shared B-tree node CoW shadowing for thin snapshots                */
/* ------------------------------------------------------------------ */

/*
 * Ensure every B-tree node along the search path from `*root_ptr` to `target_key`
 * has exclusive ownership (meta refcount == 1). Whenever a shared node (rc > 1)
 * is encountered, clone it into a fresh metadata block, decrement the old block's
 * refcount, and increment refcounts of every child entry (metadata blocks for
 * internal nodes; data blocks for bottom-tree leaf nodes).
 */
THIN_NOINLINE static lvmcore_status btree_shadow_path(thin_ctx *c,
                                                      uint64_t *root_ptr,
                                                      uint64_t target_key,
                                                      int leaf_holds_data_blocks) {
    uint64_t parent_blk = 0;
    uint32_t parent_idx = 0;
    int has_parent = 0;
    uint64_t cur = *root_ptr;

    for (unsigned depth = 0; depth < BTREE_MAX_DEPTH; depth++) {
        uint32_t rc = 0;
        lvmcore_status st = sm_get_rc(c, 1, cur, &rc);
        if (st != LVMCORE_OK) return st;

        uint8_t *node = slot_ptr(c->scratch, SLOT_BT_SHAD);
        st = meta_read_blk(c, cur, LVMCORE_THIN_BTREE_CSUM_XOR, node);
        if (st != LVMCORE_OK) return st;
        if (!btree_validate_node(node, 8u)) return LVMCORE_ERR_CORRUPT_METADATA;

        if (rc > 1u) {
            uint64_t fresh = 0;
            st = sm_alloc_meta(c, &fresh);
            if (st != LVMCORE_OK) return st;

            /* Re-read node into SLOT_BT_SHAD (alloc only touches SM slots 1..4). */
            uint32_t fl = th_le32(node + 4);
            uint32_t nr = th_le32(node + 16);
            uint32_t mx = th_le32(node + 20);

            /* Increment refcounts of all children inherited by the cloned node. */
            for (uint32_t i = 0; i < nr; i++) {
                uint64_t raw_v = th_le64(btree_val_cptr(node, mx, 8u, i));
                if (fl == BTREE_INTERNAL) {
                    st = sm_adjust_rc(c, 1, raw_v, +1);
                } else if (leaf_holds_data_blocks) {
                    st = sm_adjust_rc(c, 0, raw_v >> 24, +1);
                } else {
                    /* Top-level mapping leaf holds bottom-tree root block numbers. */
                    st = sm_adjust_rc(c, 1, raw_v, +1);
                }
                if (st != LVMCORE_OK) return st;
            }

            st = meta_write_blk(c, fresh, LVMCORE_THIN_BTREE_CSUM_XOR, node);
            if (st != LVMCORE_OK) return st;

            st = sm_adjust_rc(c, 1, cur, -1);
            if (st != LVMCORE_OK) return st;

            if (!has_parent) {
                *root_ptr = fresh;
            } else {
                uint8_t *par = slot_ptr(c->scratch, SLOT_BT_PAR);
                st = meta_read_blk(c, parent_blk, LVMCORE_THIN_BTREE_CSUM_XOR, par);
                if (st != LVMCORE_OK) return st;
                uint32_t pmx = th_le32(par + 20);
                th_put_le64(btree_val_ptr(par, pmx, 8u, parent_idx), fresh);
                st = meta_write_blk(c, parent_blk, LVMCORE_THIN_BTREE_CSUM_XOR, par);
                if (st != LVMCORE_OK) return st;
            }
            cur = fresh;
        }

        uint32_t fl = th_le32(node + 4);
        if (fl == BTREE_LEAF) return LVMCORE_OK;

        uint32_t nr = th_le32(node + 16);
        uint32_t mx = th_le32(node + 20);
        if (nr == 0) return LVMCORE_ERR_CORRUPT_METADATA;
        uint32_t pos = btree_lower_bound(node, nr, target_key);
        uint32_t cidx = (pos < nr && btree_key(node, pos) == target_key)
                            ? pos
                            : (pos > 0 ? pos - 1u : 0u);
        parent_blk = cur;
        parent_idx = cidx;
        has_parent = 1;
        cur = th_le64(btree_val_cptr(node, mx, 8u, cidx));
    }
    return LVMCORE_ERR_CORRUPT_METADATA;
}

/* Check if any metadata node on the search path to `target_key` has refcount > 1. */
THIN_NOINLINE static lvmcore_status btree_path_is_shared(thin_ctx *c, uint64_t root,
                                                         uint64_t target_key,
                                                         int *shared_out) {
    *shared_out = 0;
    uint64_t cur = root;
    uint8_t *node = slot_ptr(c->scratch, SLOT_BT_CUR);
    for (unsigned depth = 0; depth < BTREE_MAX_DEPTH; depth++) {
        uint32_t rc = 0;
        lvmcore_status st = sm_get_rc(c, 1, cur, &rc);
        if (st != LVMCORE_OK) return st;
        if (rc > 1u) {
            *shared_out = 1;
            return LVMCORE_OK;
        }
        st = meta_read_blk(c, cur, LVMCORE_THIN_BTREE_CSUM_XOR, node);
        if (st != LVMCORE_OK) return st;
        if (!btree_validate_node(node, 8u)) return LVMCORE_ERR_CORRUPT_METADATA;
        if (th_le32(node + 4) == BTREE_LEAF) return LVMCORE_OK;
        uint32_t nr = th_le32(node + 16);
        uint32_t mx = th_le32(node + 20);
        if (nr == 0) return LVMCORE_ERR_CORRUPT_METADATA;
        uint32_t pos = btree_lower_bound(node, nr, target_key);
        uint32_t cidx = (pos < nr && btree_key(node, pos) == target_key)
                            ? pos
                            : (pos > 0 ? pos - 1u : 0u);
        cur = th_le64(btree_val_cptr(node, mx, 8u, cidx));
    }
    return LVMCORE_ERR_CORRUPT_METADATA;
}

/* ------------------------------------------------------------------ */
/* Public Read & Write entry points                                   */
/* ------------------------------------------------------------------ */

static int th_data_blk_off(uint64_t data_blk, uint64_t chunk_bytes,
                           uint64_t intra, size_t step, uint64_t tdata_size,
                           uint64_t *off_out) {
    if (chunk_bytes == 0 || tdata_size < chunk_bytes) return 0;
    if (data_blk > (tdata_size - chunk_bytes) / chunk_bytes) return 0;
    if (intra > chunk_bytes || (uint64_t)step > chunk_bytes - intra) return 0;
    *off_out = data_blk * chunk_bytes + intra;
    return 1;
}

THIN_NOINLINE lvmcore_status lvmcore_thin_read(const lvmcore_rw_source *tmeta,
                                               const lvmcore_rw_source *tdata,
                                               uint64_t dev_id,
                                               uint8_t *meta_scratch,
                                               size_t meta_scratch_cap,
                                               uint64_t offset,
                                               void *dst,
                                               size_t len) {
    if (!tmeta || !tmeta->read || !tdata || !tdata->read || !meta_scratch ||
        meta_scratch_cap < LVMCORE_THIN_SCRATCH_META_BYTES || (!dst && len > 0)) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;
    if ((uint64_t)len > UINT64_MAX - offset) return LVMCORE_ERR_INVALID_ARG;

    thin_ctx ctx;
    th_zero(&ctx, sizeof(ctx));
    ctx.tmeta = tmeta;
    ctx.tdata = tdata;
    ctx.scratch = meta_scratch;

    lvmcore_status st = lvmcore_thin_read_superblock(tmeta, slot_ptr(meta_scratch, SLOT_SB), &ctx.sb);
    if (st != LVMCORE_OK) return st;

    uint64_t chunk_bytes = (uint64_t)ctx.sb.data_block_size * 512ULL;
    if (chunk_bytes == 0 || ctx.sb.data_sm.nr_blocks > tdata->size / chunk_bytes) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }

    /* Verify dev_id exists in data_mapping_root -> bottom root blocknr. */
    uint8_t dev_root_raw[8];
    int found_dev = btree_lookup(&ctx, ctx.sb.data_mapping_root, dev_id, 8u,
                                 SLOT_BT_CUR, dev_root_raw, &st);
    if (found_dev < 0) return st;
    if (found_dev == 0) {
        /* Verify device exists in details tree; if absent, invalid device_id. */
        uint8_t det[24];
        int fd2 = btree_lookup(&ctx, ctx.sb.device_details_root, dev_id, 24u,
                               SLOT_DEV_DET, det, &st);
        if (fd2 <= 0) return (fd2 < 0) ? st : LVMCORE_ERR_CORRUPT_METADATA;
        th_zero(dst, len);
        return LVMCORE_OK;
    }

    uint64_t dev_root = th_le64(dev_root_raw);
    uint8_t *out = (uint8_t *)dst;
    uint64_t cur = offset;
    size_t rem = len;

    while (rem > 0) {
        uint64_t vblock = cur / chunk_bytes;
        uint64_t intra = cur % chunk_bytes;
        uint64_t avail = chunk_bytes - intra;
        size_t step = ((uint64_t)rem < avail) ? rem : (size_t)avail;

        uint8_t map_raw[8];
        int mapped = btree_lookup(&ctx, dev_root, vblock, 8u, SLOT_BT_CUR, map_raw, &st);
        if (mapped < 0) return st;

        if (mapped == 0) {
            th_zero(out, step);
        } else {
            uint64_t data_blk = th_le64(map_raw) >> 24;
            uint64_t data_off = 0;
            if (data_blk >= ctx.sb.data_sm.nr_blocks ||
                !th_data_blk_off(data_blk, chunk_bytes, intra, step, tdata->size, &data_off)) {
                return LVMCORE_ERR_CORRUPT_METADATA;
            }
            if (tdata->read(tdata->impl, data_off, out, step) != 0) return LVMCORE_ERR_IO;
        }

        cur += step;
        out += step;
        rem -= step;
    }
    return LVMCORE_OK;
}

THIN_NOINLINE static lvmcore_status thin_write_one_chunk(thin_ctx *ctx,
                                                         uint64_t dev_id,
                                                         uint64_t *dev_root_ptr,
                                                         uint64_t *mapped_blocks_ptr,
                                                         int *meta_dirty,
                                                         uint64_t vblock,
                                                         uint64_t intra,
                                                         const uint8_t *in,
                                                         size_t step,
                                                         uint8_t *chunk_buf,
                                                         uint64_t chunk_bytes,
                                                         int zero_new_blocks) {
    lvmcore_status st = LVMCORE_OK;
    uint8_t map_raw[8];
    int mapped = btree_lookup(ctx, *dev_root_ptr, vblock, 8u, SLOT_BT_CUR, map_raw, &st);
    if (mapped < 0) return st;

    if (mapped == 1) {
        uint64_t old_db = th_le64(map_raw) >> 24;
        if (old_db >= ctx->sb.data_sm.nr_blocks) return LVMCORE_ERR_CORRUPT_METADATA;

        /* Fast path: write in-place if path is unshared and data block rc == 1. */
        int top_shared = 0, bot_shared = 0;
        st = btree_path_is_shared(ctx, ctx->sb.data_mapping_root, dev_id, &top_shared);
        if (st != LVMCORE_OK) return st;
        st = btree_path_is_shared(ctx, *dev_root_ptr, vblock, &bot_shared);
        if (st != LVMCORE_OK) return st;
        uint32_t db_rc = 0;
        st = sm_get_rc(ctx, 0, old_db, &db_rc);
        if (st != LVMCORE_OK) return st;

        if (!top_shared && !bot_shared && db_rc == 1u) {
            uint64_t data_off = 0;
            if (!th_data_blk_off(old_db, chunk_bytes, intra, step, ctx->tdata->size, &data_off)) {
                return LVMCORE_ERR_CORRUPT_METADATA;
            }
            return (ctx->tdata->write(ctx->tdata->impl, data_off, in, step) == 0)
                       ? LVMCORE_OK
                       : LVMCORE_ERR_IO;
        }

        /* Shadow shared metadata path first so all child data refcounts are exact. */
        if (top_shared) {
            st = btree_shadow_path(ctx, &ctx->sb.data_mapping_root, dev_id, 0);
            if (st != LVMCORE_OK) return st;
            *meta_dirty = 1;
        }
        if (bot_shared) {
            uint64_t prev_root = *dev_root_ptr;
            st = btree_shadow_path(ctx, dev_root_ptr, vblock, 1);
            if (st != LVMCORE_OK) return st;
            if (*dev_root_ptr != prev_root) {
                uint8_t dr_bytes[8];
                th_put_le64(dr_bytes, *dev_root_ptr);
                st = btree_upsert_simple(ctx, &ctx->sb.data_mapping_root, dev_id, dr_bytes, 8u,
                                         SLOT_BT_CUR, SLOT_BT_SIB, SLOT_BT_PAR, NULL);
                if (st != LVMCORE_OK) return st;
            }
            *meta_dirty = 1;
        }

        /* Re-check data block refcount after shadowing. */
        st = sm_get_rc(ctx, 0, old_db, &db_rc);
        if (st != LVMCORE_OK) return st;
        if (db_rc == 1u) {
            uint64_t data_off = 0;
            if (!th_data_blk_off(old_db, chunk_bytes, intra, step, ctx->tdata->size, &data_off)) {
                return LVMCORE_ERR_CORRUPT_METADATA;
            }
            return (ctx->tdata->write(ctx->tdata->impl, data_off, in, step) == 0)
                       ? LVMCORE_OK
                       : LVMCORE_ERR_IO;
        }

        /* CoW shared data block: allocate new data block, copy-modify-write, dec old rc. */
        uint64_t new_db = 0;
        st = sm_alloc_block(ctx, 0, &new_db);
        if (st != LVMCORE_OK) return st;

        uint64_t old_base = 0, new_base = 0;
        if (!th_data_blk_off(old_db, chunk_bytes, 0, (size_t)chunk_bytes, ctx->tdata->size, &old_base) ||
            !th_data_blk_off(new_db, chunk_bytes, 0, (size_t)chunk_bytes, ctx->tdata->size, &new_base)) {
            return LVMCORE_ERR_CORRUPT_METADATA;
        }

        if (step < (size_t)chunk_bytes) {
            if (ctx->tdata->read(ctx->tdata->impl, old_base, chunk_buf,
                                 (size_t)chunk_bytes) != 0) {
                return LVMCORE_ERR_IO;
            }
            th_memcpy(chunk_buf + intra, in, step);
            if (ctx->tdata->write(ctx->tdata->impl, new_base, chunk_buf,
                                  (size_t)chunk_bytes) != 0) {
                return LVMCORE_ERR_IO;
            }
        } else {
            if (ctx->tdata->write(ctx->tdata->impl, new_base, in,
                                  (size_t)chunk_bytes) != 0) {
                return LVMCORE_ERR_IO;
            }
        }

        st = sm_adjust_rc(ctx, 0, old_db, -1);
        if (st != LVMCORE_OK) return st;

        uint64_t packed = (new_db << 24) | ((uint64_t)ctx->sb.time & 0xffffffULL);
        uint8_t new_raw[8];
        th_put_le64(new_raw, packed);
        st = btree_upsert_simple(ctx, dev_root_ptr, vblock, new_raw, 8u,
                                 SLOT_BT_CUR, SLOT_BT_SIB, SLOT_BT_PAR, NULL);
        if (st != LVMCORE_OK) return st;
        *meta_dirty = 1;
        return LVMCORE_OK;
    }

    /* Unmapped chunk: shadow shared path, allocate new data block, insert mapping. */
    st = btree_shadow_path(ctx, &ctx->sb.data_mapping_root, dev_id, 0);
    if (st != LVMCORE_OK) return st;
    uint64_t prev_root = *dev_root_ptr;
    st = btree_shadow_path(ctx, dev_root_ptr, vblock, 1);
    if (st != LVMCORE_OK) return st;
    if (*dev_root_ptr != prev_root) {
        uint8_t dr_bytes[8];
        th_put_le64(dr_bytes, *dev_root_ptr);
        st = btree_upsert_simple(ctx, &ctx->sb.data_mapping_root, dev_id, dr_bytes, 8u,
                                 SLOT_BT_CUR, SLOT_BT_SIB, SLOT_BT_PAR, NULL);
        if (st != LVMCORE_OK) return st;
    }

    uint64_t new_db = 0;
    st = sm_alloc_block(ctx, 0, &new_db);
    if (st != LVMCORE_OK) return st;

    uint64_t new_base = 0;
    if (!th_data_blk_off(new_db, chunk_bytes, 0, (size_t)chunk_bytes, ctx->tdata->size, &new_base)) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }

    /* Always zero unoverwritten regions of newly allocated chunks so scratch memory never leaks. */
    (void)zero_new_blocks;
    if (step < (size_t)chunk_bytes) {
        th_zero(chunk_buf, (size_t)chunk_bytes);
        th_memcpy(chunk_buf + intra, in, step);
        if (ctx->tdata->write(ctx->tdata->impl, new_base, chunk_buf,
                              (size_t)chunk_bytes) != 0) {
            return LVMCORE_ERR_IO;
        }
    } else {
        if (ctx->tdata->write(ctx->tdata->impl, new_base, in,
                              (size_t)chunk_bytes) != 0) {
            return LVMCORE_ERR_IO;
        }
    }

    uint64_t packed = (new_db << 24) | ((uint64_t)ctx->sb.time & 0xffffffULL);
    uint8_t new_raw[8];
    th_put_le64(new_raw, packed);
    int inserted = 0;
    st = btree_upsert_simple(ctx, dev_root_ptr, vblock, new_raw, 8u,
                             SLOT_BT_CUR, SLOT_BT_SIB, SLOT_BT_PAR, &inserted);
    if (st != LVMCORE_OK) return st;
    if (inserted) (*mapped_blocks_ptr)++;
    *meta_dirty = 1;
    return LVMCORE_OK;
}

THIN_NOINLINE lvmcore_status lvmcore_thin_write(const lvmcore_rw_source *tmeta,
                                                const lvmcore_rw_source *tdata,
                                                uint64_t dev_id,
                                                uint8_t *meta_scratch,
                                                size_t meta_scratch_cap,
                                                uint8_t *chunk_buf,
                                                size_t chunk_buf_cap,
                                                int zero_new_blocks,
                                                uint64_t offset,
                                                const void *src,
                                                size_t len) {
    if (!tmeta || !tmeta->read || !tmeta->write || !tdata || !tdata->read || !tdata->write ||
        !meta_scratch || meta_scratch_cap < LVMCORE_THIN_SCRATCH_META_BYTES ||
        !chunk_buf || (!src && len > 0)) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;
    if ((uint64_t)len > UINT64_MAX - offset) return LVMCORE_ERR_INVALID_ARG;

    thin_ctx ctx;
    th_zero(&ctx, sizeof(ctx));
    ctx.tmeta = tmeta;
    ctx.tdata = tdata;
    ctx.scratch = meta_scratch;

    lvmcore_status st = lvmcore_thin_read_superblock(tmeta, slot_ptr(meta_scratch, SLOT_SB), &ctx.sb);
    if (st != LVMCORE_OK) return st;
    if (ctx.sb.metadata_snap != 0) return LVMCORE_ERR_UNSUPPORTED;

    uint64_t chunk_bytes = (uint64_t)ctx.sb.data_block_size * 512ULL;
    if (chunk_bytes == 0 || chunk_bytes > (uint64_t)chunk_buf_cap) {
        return LVMCORE_ERR_SCRATCH_EXHAUSTED;
    }
    if (ctx.sb.data_sm.nr_blocks > tdata->size / chunk_bytes) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }

    uint8_t det_raw[24];
    int found_det = btree_lookup(&ctx, ctx.sb.device_details_root, dev_id, 24u,
                                 SLOT_DEV_DET, det_raw, &st);
    if (found_det <= 0) return (found_det < 0) ? st : LVMCORE_ERR_CORRUPT_METADATA;
    uint64_t mapped_blocks = th_le64(det_raw);
    uint64_t orig_mapped_blocks = mapped_blocks;

    uint8_t dr_raw[8];
    int found_dev = btree_lookup(&ctx, ctx.sb.data_mapping_root, dev_id, 8u,
                                 SLOT_BT_CUR, dr_raw, &st);
    if (found_dev <= 0) return (found_dev < 0) ? st : LVMCORE_ERR_CORRUPT_METADATA;
    uint64_t dev_root = th_le64(dr_raw);
    uint64_t orig_dev_root = dev_root;

    int meta_dirty = 0;
    const uint8_t *in = (const uint8_t *)src;
    uint64_t cur = offset;
    size_t rem = len;

    while (rem > 0) {
        uint64_t vblock = cur / chunk_bytes;
        uint64_t intra = cur % chunk_bytes;
        uint64_t avail = chunk_bytes - intra;
        size_t step = ((uint64_t)rem < avail) ? rem : (size_t)avail;

        st = thin_write_one_chunk(&ctx, dev_id, &dev_root, &mapped_blocks, &meta_dirty,
                                  vblock, intra, in, step, chunk_buf, chunk_bytes,
                                  zero_new_blocks);
        if (st != LVMCORE_OK) return st;

        cur += step;
        in += step;
        rem -= step;
    }

    if (meta_dirty) {
        if (dev_root != orig_dev_root) {
            th_put_le64(dr_raw, dev_root);
            st = btree_upsert_simple(&ctx, &ctx.sb.data_mapping_root, dev_id, dr_raw, 8u,
                                     SLOT_BT_CUR, SLOT_BT_SIB, SLOT_BT_PAR, NULL);
            if (st != LVMCORE_OK) return st;
        }
        if (mapped_blocks != orig_mapped_blocks) {
            th_put_le64(det_raw, mapped_blocks);
            st = btree_upsert_simple(&ctx, &ctx.sb.device_details_root, dev_id, det_raw, 24u,
                                     SLOT_DEV_DET, SLOT_BT_SIB, SLOT_BT_PAR, NULL);
            if (st != LVMCORE_OK) return st;
        }
        st = flush_superblock(&ctx);
        if (st != LVMCORE_OK) return st;
    }
    return LVMCORE_OK;
}
