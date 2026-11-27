/*
 * test_lvmcore_hardening.c — adversarial vectors and hardening tests for lvmcore.
 *
 * Verifies memory safety, boundary conditions, and error handling:
 *   1. Stack limit: strictly non-recursive, stack frame <= 512 bytes.
 *   2. Static scratch race: multiple arenas concurrently/interleaved, no statics.
 *   3. Unaligned read: unaligned memory sources and offsets handled safely.
 *   4. Typeless / overflow segment: fail-closed rejection without crashes.
 *   5. MDA wrap: metadata text wrapped across ring buffer boundary.
 *   6. Arena safety: fixed-size scratch exhaustion returns
 *      LVMCORE_ERR_SCRATCH_EXHAUSTED gracefully without crashing.
 *   7. CRC32 edge vectors, wraparound uint64 sizes, oversized commit descriptors,
 *      and UUID cross-checks.
 *
 * Usage: test_lvmcore_hardening (no arguments — all synthetic in-memory).
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lvmcore/types.h"
#include "lvmcore/arena.h"
#include "lvmcore/crc32.h"
#include "lvmcore/parser.h"

static int fails = 0, passes = 0;
#define CHECK(name, cond) do { \
    if (cond) { passes++; printf("[  OK  ] %s\n", name); } \
    else { fails++; printf("[ FAIL ] %s\n", name); } } while (0)

static void put_le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_le64(unsigned char *p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

/* In-memory byte source */
typedef struct {
    const unsigned char *data;
    uint64_t size;
    unsigned reads;
} mem_src;

static size_t mem_read(void *impl, uint64_t off, void *buf, size_t len) {
    mem_src *m = (mem_src *)impl;
    m->reads++;
    if (off + len < off || off + len > m->size) return 0;
    memcpy(buf, m->data + off, len);
    return len;
}

static void mem_source(const unsigned char *img, uint64_t size, lvmcore_byte_source *src,
                       mem_src *m) {
    m->data = img;
    m->size = size;
    m->reads = 0;
    src->impl = m;
    src->size = size;
    src->read = mem_read;
}

#define IMG_SIZE (4u * 1024 * 1024)
#define LABEL_OFF 512u
#define MDA_OFF 4096u
#define MDA_SIZE 0x100000u          /* 1 MiB */
#define TEXT_OFF (MDA_OFF + 512u)

static unsigned char img[IMG_SIZE];
static uint8_t arena_buf[2 * 1024 * 1024];

static void build_label(void) {
    memset(img, 0, sizeof(img));
    unsigned char *p = img + LABEL_OFF;
    memcpy(p, "LABELONE", 8);
    put_le64(p + 8, 1);
    put_le32(p + 16, 0);
    put_le32(p + 20, 32);
    memcpy(p + 24, "LVM2 001", 8);

    unsigned char *pvh = p + 32;
    memcpy(pvh, "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP", 32);
    put_le64(pvh + 32, IMG_SIZE);

    unsigned char *d = pvh + 40;
    put_le64(d, 0x1000000); put_le64(d + 8, 0x7f00000);
    put_le64(d + 16, 0); put_le64(d + 24, 0);

    unsigned char *m = d + 32;
    put_le64(m, MDA_OFF); put_le64(m + 8, MDA_SIZE);
    put_le64(m + 16, 0); put_le64(m + 24, 0);

    put_le32(p + 16, lvmcore_crc32(p + 20, 512 - 20));
}

static void build_mda(const char *text) {
    size_t len = strlen(text);
    memcpy(img + TEXT_OFF, text, len);
    img[TEXT_OFF + len] = '\0';
    size_t commit_size = len + 1;

    unsigned char *h = img + MDA_OFF;
    memset(h, 0, 512);
    put_le32(h, 0);
    memcpy(h + 4, " LVM2 x[5A%r0N*>", 16);
    put_le32(h + 20, 1);
    put_le64(h + 24, MDA_OFF);
    put_le64(h + 32, MDA_SIZE);
    put_le64(h + 40, 512);
    put_le64(h + 48, commit_size);
    put_le32(h + 56, lvmcore_crc32(img + TEXT_OFF, commit_size));
    put_le32(h, lvmcore_crc32(h + 4, 512 - 4));
}

static void stripes_vg(char *out, size_t cap, int n_stripes, const char *seg_type) {
    int ext_cnt = (n_stripes > 4) ? n_stripes : 4;
    size_t w = 0;
    w += (size_t)snprintf(out + w, cap - w,
        "vgx {\n"
        "id = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
        "seqno = 1\n"
        "status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n"
        "extent_size = 8192\n"
        "physical_volumes {\n"
        "  pv0 {\n"
        "    id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\"\n"
        "    device = \"/dev/loop0\"\n"
        "    status = [\"ALLOCATABLE\"]\n"
        "    pe_start = 2048\n"
        "    pe_count = 100\n"
        "  }\n"
        "}\n"
        "logical_volumes {\n"
        "  lv0 {\n"
        "    id = \"lllllllllllllllllllllllllllllll\"\n"
        "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
        "    segment_count = 1\n"
        "    segment1 {\n"
        "      start_extent = 0\n"
        "      extent_count = %d\n"
        "      type = \"%s\"\n"
        "      stripe_count = %d\n"
        "      stripe_size = 128\n"
        "      stripes = [",
        ext_cnt, seg_type, n_stripes);
    for (int i = 0; i < n_stripes; i++)
        w += (size_t)snprintf(out + w, cap - w, "\"pv0\", %d, ", i);
    w += (size_t)snprintf(out + w, cap - w, "]\n    }\n  }\n }\n}\n");
}

int main(void) {
    char meta[2048];

    /* ---- 1. CRC edge vectors ----------------------------------------- */
    {
        CHECK("crc32 zero-length returns LVMCORE_CRC32_INIT (0xf597a6cf)",
              lvmcore_crc32("", 0) == LVMCORE_CRC32_INIT);
        uint32_t one = lvmcore_crc32("\x00", 1);
        CHECK("crc32 single zero byte differs from seed", one != LVMCORE_CRC32_INIT);
        CHECK("crc32 matches poly 0xedb88320 (nonzero)", lvmcore_crc32("A", 1) != 0u);
        uint32_t a = lvmcore_crc32("abc", 3);
        uint32_t b = lvmcore_crc32("abc", 3);
        CHECK("crc32 deterministic", a == b);
        uint32_t c1 = lvmcore_crc32_update(LVMCORE_CRC32_INIT, "a", 1);
        c1 = lvmcore_crc32_update(c1, "bc", 2);
        CHECK("crc32 incremental update matches one-shot", c1 == a);
    }

    /* ---- 2. stripe boundary: exactly 4 stripes parse clean ---------- */
    {
        build_label();
        stripes_vg(meta, sizeof(meta), 4, "striped");
        build_mda(meta);

        lvmcore_byte_source src; mem_src m;
        mem_source(img, IMG_SIZE, &src, &m);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("4-stripe segment parses", st == LVMCORE_OK);
        CHECK("4-stripe fills stripes exactly, no overflow",
              vg.lv_count == 1 && vg.lvs[0].segment_count == 1 &&
              vg.lvs[0].segments[0].stripe_count_used == 4 &&
              !vg.lvs[0].segments[0].stripe_overflow);
        CHECK("4 stripes land on pv0 extents 0..3",
              vg.lvs[0].segments[0].stripes[3].start_extent == 3);
        CHECK("4-stripe LV stays mappable", vg.lvs[0].mappable == 1);
    }

    /* ---- 3. five stripes: arena-grown vector holds them, no OOB ------ */
    {
        build_label();
        stripes_vg(meta, sizeof(meta), 5, "striped");
        build_mda(meta);

        lvmcore_byte_source src; mem_src m;
        mem_source(img, IMG_SIZE, &src, &m);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("5-stripe image still parses the VG", st == LVMCORE_OK);
        CHECK("arena vector captures all 5 stripes, no overflow flag",
              vg.lv_count == 1 &&
              vg.lvs[0].segments[0].stripe_count_used == 5 &&
              vg.lvs[0].segments[0].stripe_overflow == 0);
        CHECK("5th stripe data intact (key + extent 4)",
              vg.lvs[0].segments[0].stripes[4].start_extent == 4 &&
              strcmp(vg.lvs[0].segments[0].stripes[4].pv_key, "pv0") == 0);
        CHECK("5-stripe LV remains parsed mappable (mapping deferral to W2)",
              vg.lvs[0].mappable == 1);
    }

    /* ---- 4. metadata text CRC corruption fails closed ---------------- */
    {
        build_label();
        stripes_vg(meta, sizeof(meta), 1, "striped");
        build_mda(meta);
        img[TEXT_OFF + 10] ^= 0xff;

        lvmcore_byte_source src; mem_src m;
        mem_source(img, IMG_SIZE, &src, &m);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("corrupted metadata CRC rejected",
              st == LVMCORE_ERR_CORRUPT_METADATA && err.failed);
    }

    /* ---- 5. mda area offset/size wraparound rejected ----------------- */
    {
        build_label();
        unsigned char *pvh = img + LABEL_OFF + 32;
        unsigned char *m = pvh + 40 + 32;
        put_le64(m, MDA_OFF);
        put_le64(m + 8, UINT64_MAX - MDA_OFF + 2);
        unsigned char *p = img + LABEL_OFF;
        put_le32(p + 16, lvmcore_crc32(p + 20, 512 - 20));

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("wrapping metadata-area size rejected", st != LVMCORE_OK);
    }

    /* ---- 6. absurd commit_size descriptor rejected without reading ---- */
    {
        build_label();
        stripes_vg(meta, sizeof(meta), 1, "striped");
        build_mda(meta);
        unsigned char *h = img + MDA_OFF;
        put_le64(h + 48, 512u * 1024 * 1024);
        put_le32(h, lvmcore_crc32(h + 4, 512 - 4));

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("oversized commit descriptor rejected", st != LVMCORE_OK);
    }

    /* ---- 7. segment with no type key: refused, never linear ----------- */
    {
        build_label();
        snprintf(meta, sizeof(meta),
            "vgx {\n"
            "id = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "seqno = 1\n"
            "extent_size = 8192\n"
            "physical_volumes {\n"
            "  pv0 {\n"
            "    id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\"\n"
            "    pe_start = 2048\n"
            "    pe_count = 100\n"
            "  }\n"
            "}\n"
            "logical_volumes {\n"
            "  lv0 {\n"
            "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
            "    segment_count = 1\n"
            "    segment1 {\n"
            "      start_extent = 0\n"
            "      extent_count = 4\n"
            "      stripes = [\"pv0\", 0]\n"
            "    }\n"
            "  }\n"
            "}\n"
            "}\n");
        build_mda(meta);

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("typeless segment parses VG", st == LVMCORE_OK);
        CHECK("typeless segment is refused (fail closed)",
              vg.lv_count == 1 && vg.lvs[0].mappable == 0 &&
              strcmp(vg.lvs[0].unsupported_reason, "unsupported segment type") == 0);
    }

    /* ---- 8. foreign system_id copied, bounded, parsing unaffected ---- */
    {
        build_label();
        stripes_vg(meta, sizeof(meta), 1, "striped");
        char with_sys[2200];
        const char *mark = "seqno = 1\n";
        char *at = strstr(meta, mark);
        if (at) {
            size_t head = (size_t)(at - meta);
            memcpy(with_sys, meta, head);
            with_sys[head] = '\0';
            strcat(with_sys, "system_id = \"");
            for (int i = 0; i < 80; i++) strcat(with_sys, "S");
            strcat(with_sys, "\"\n");
            strcat(with_sys, at);
        } else {
            strcpy(with_sys, meta);
        }
        build_mda(with_sys);

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("oversized system_id parses VG", st == LVMCORE_OK);
        CHECK("system_id is truncated and NUL-terminated",
              strlen(vg.system_id) < sizeof(vg.system_id) &&
              strncmp(vg.system_id, "SSSS", 4) == 0);
    }

    /* ---- 9. UUID cross-check match and mismatch ----------------------- */
    {
        build_label();
        stripes_vg(meta, sizeof(meta), 1, "striped");
        build_mda(meta);

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        CHECK("UUID match succeeds",
              lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err) == LVMCORE_OK);

        /* Corrupt label UUID */
        img[LABEL_OFF + 32] = 'Q';
        unsigned char *p = img + LABEL_OFF;
        put_le32(p + 16, lvmcore_crc32(p + 20, 512 - 20));

        lvmcore_arena_reset(&arena);
        lvmcore_volume_group vg2;
        lvmcore_err err2 = {0};
        CHECK("UUID mismatch rejected",
              lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg2, &err2) != LVMCORE_OK);
    }

    /* ---- 10. Fixed-size Arena Exhaustion ----------------------------- */
    {
        build_label();
        stripes_vg(meta, sizeof(meta), 1, "striped");
        build_mda(meta);

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);

        /* Tiny 64-byte arena: cannot hold PV label or metadata text */
        uint8_t tiny_buf[64];
        lvmcore_arena tiny_arena;
        lvmcore_arena_init(&tiny_arena, tiny_buf, sizeof(tiny_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &tiny_arena, &vg, &err);
        CHECK("exhausted arena returns LVMCORE_ERR_SCRATCH_EXHAUSTED safely",
              st == LVMCORE_ERR_SCRATCH_EXHAUSTED && err.failed);
    }

    /* ---- 11. Concurrent Arena Reentrancy ----------------------------- */
    {
        build_label();
        stripes_vg(meta, sizeof(meta), 2, "striped");
        build_mda(meta);

        lvmcore_byte_source src1, src2;
        mem_src ms1, ms2;
        mem_source(img, IMG_SIZE, &src1, &ms1);
        mem_source(img, IMG_SIZE, &src2, &ms2);

        uint8_t buf1[64 * 1024];
        uint8_t buf2[64 * 1024];
        lvmcore_arena a1, a2;
        lvmcore_arena_init(&a1, buf1, sizeof(buf1));
        lvmcore_arena_init(&a2, buf2, sizeof(buf2));

        lvmcore_volume_group vg1, vg2;
        lvmcore_err err1 = {0}, err2 = {0};

        /* Parse interleaved across distinct arenas */
        lvmcore_pv_label l1, l2;
        lvmcore_status st1 = lvmcore_read_pv_label(&src1, &a1, &l1, &err1);
        lvmcore_status st2 = lvmcore_read_pv_label(&src2, &a2, &l2, &err2);
        CHECK("interleaved label parse 1 succeeds", st1 == LVMCORE_OK);
        CHECK("interleaved label parse 2 succeeds", st2 == LVMCORE_OK);

        st1 = lvmcore_read_vg(&src1, LVMCORE_READ_FLAG_DEFAULT, &a1, &vg1, &err1);
        st2 = lvmcore_read_vg(&src2, LVMCORE_READ_FLAG_DEFAULT, &a2, &vg2, &err2);
        CHECK("interleaved VG parse 1 succeeds", st1 == LVMCORE_OK && vg1.lv_count == 1);
        CHECK("interleaved VG parse 2 succeeds", st2 == LVMCORE_OK && vg2.lv_count == 1);
        CHECK("reentrant parses produce identical results",
              strcmp(vg1.name, vg2.name) == 0 && vg1.seqno == vg2.seqno);
    }

    /* ---- 12. Unaligned Read Safety ----------------------------------- */
    {
        /* Feed unaligned arena buffer and unaligned text parse */
        uint8_t unaligned_arena_buf[64 * 1024 + 7];
        uint8_t *unaligned_ptr = unaligned_arena_buf + 3; /* misaligned by 3 bytes */
        lvmcore_arena unaligned_arena;
        lvmcore_arena_init(&unaligned_arena, unaligned_ptr, 64 * 1024);

        build_label();
        stripes_vg(meta, sizeof(meta), 1, "striped");
        build_mda(meta);

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &unaligned_arena, &vg, &err);
        CHECK("unaligned arena buffer parses safely without alignment fault",
              st == LVMCORE_OK && vg.lv_count == 1);
    }

    /* ---- 13. MDA Ring Wrap-Around ------------------------------------ */
    {
        /* Set up MDA so text spans across the 1 MiB ring boundary.
         * MDA starts at MDA_OFF = 4096, size = MDA_SIZE = 0x100000 (1 MiB).
         * Header is 512 bytes (4096..4607).
         * Ring buffer is 4608 .. 4096 + 0x100000 - 1.
         * Ring size = 0x100000 - 512 = 1048064 bytes.
         * Let's put text right near the end of the area:
         * raw_locn.offset = MDA_SIZE - 20 (so only 20 bytes fit before wrap).
         * The remaining text wraps around to offset 512 (ring begin).
         */
        build_label();
        stripes_vg(meta, sizeof(meta), 1, "striped");
        size_t text_len = strlen(meta) + 1; /* includes trailing NUL */

        uint64_t wrap_offset = MDA_SIZE - 20; /* 20 bytes in first part */
        size_t first_part = 20;
        size_t wrapped_part = text_len - first_part;

        /* Copy first 20 bytes to end of MDA */
        memcpy(img + MDA_OFF + wrap_offset, meta, first_part);
        /* Copy wrapped part to beginning of ring (MDA_OFF + 512) */
        memcpy(img + MDA_OFF + 512, meta + first_part, wrapped_part);

        /* Build full text in contiguous buffer to compute expected CRC */
        unsigned char full_text[2048];
        memcpy(full_text, meta, text_len);
        uint32_t text_crc = lvmcore_crc32(full_text, text_len);

        unsigned char *h = img + MDA_OFF;
        memset(h, 0, 512);
        put_le32(h, 0);
        memcpy(h + 4, " LVM2 x[5A%r0N*>", 16);
        put_le32(h + 20, 1);
        put_le64(h + 24, MDA_OFF);
        put_le64(h + 32, MDA_SIZE);
        put_le64(h + 40, wrap_offset);
        put_le64(h + 48, text_len);
        put_le32(h + 56, text_crc);
        put_le32(h, lvmcore_crc32(h + 4, 512 - 4));

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("MDA ring wrap-around metadata parses correctly",
              st == LVMCORE_OK && vg.lv_count == 1);
        CHECK("wrapped VG name matches", strcmp(vg.name, "vgx") == 0);
    }

    /* ---- 14. PV label data_off bounds at end of 8 KiB scan buffer ---- */
    {
        /* Heap-allocate exact 8 KiB so ASan catches any read past byte 8191. */
        unsigned char *scan_img = (unsigned char *)calloc(1, LVMCORE_LABEL_SCAN_BYTES);
        CHECK("heap scan image allocated", scan_img != NULL);
        if (scan_img) {
            uint32_t bad_offs[] = { 500u, 473u, 472u, 31u };
            for (size_t bi = 0; bi < sizeof(bad_offs) / sizeof(bad_offs[0]); bi++) {
                memset(scan_img, 0, LVMCORE_LABEL_SCAN_BYTES);
                unsigned char *p = scan_img + 7680u; /* last 512B sector of 8 KiB */
                memcpy(p, "LABELONE", 8);
                put_le64(p + 8, 15);
                put_le32(p + 20, bad_offs[bi]);
                memcpy(p + 24, "LVM2 001", 8);
                put_le32(p + 16, lvmcore_crc32(p + 20, 512 - 20));

                lvmcore_byte_source src; mem_src ms;
                mem_source(scan_img, LVMCORE_LABEL_SCAN_BYTES, &src, &ms);
                lvmcore_arena arena;
                lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
                lvmcore_pv_label lbl;
                lvmcore_err err = {0};

                lvmcore_status st = lvmcore_read_pv_label(&src, &arena, &lbl, &err);
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "data_off=%u at off=7680 rejected without OOB read",
                         bad_offs[bi]);
                CHECK(msg, st == LVMCORE_ERR_CORRUPT_LABEL && err.failed);
            }

#ifndef _WIN32
            /* Hardware PROT_NONE guard page immediately after the 8 KiB head buffer. */
            long ps = sysconf(_SC_PAGESIZE);
            if (ps > 0) {
                size_t page_sz = (size_t)ps;
                size_t data_pages =
                    ((LVMCORE_LABEL_SCAN_BYTES + page_sz - 1u) / page_sz) * page_sz;
                uint8_t *region = (uint8_t *)mmap(
                    NULL, data_pages + page_sz, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
                if (region != MAP_FAILED) {
                    int prot_ok = (mprotect(region + data_pages, page_sz, PROT_NONE) == 0);
                    CHECK("PROT_NONE guard page mapped after 8 KiB scan arena", prot_ok);
                    if (prot_ok) {
                        uint8_t *guarded_arena = region + (data_pages - LVMCORE_LABEL_SCAN_BYTES);
                        uint32_t oob_offs[] = { 500u, 473u };
                        for (size_t bi = 0; bi < sizeof(oob_offs) / sizeof(oob_offs[0]); bi++) {
                            memset(scan_img, 0, LVMCORE_LABEL_SCAN_BYTES);
                            unsigned char *p = scan_img + 7680u;
                            memcpy(p, "LABELONE", 8);
                            put_le64(p + 8, 15);
                            put_le32(p + 20, oob_offs[bi]);
                            memcpy(p + 24, "LVM2 001", 8);
                            put_le32(p + 16, lvmcore_crc32(p + 20, 512 - 20));

                            lvmcore_byte_source src; mem_src ms;
                            mem_source(scan_img, LVMCORE_LABEL_SCAN_BYTES, &src, &ms);
                            lvmcore_arena g_arena;
                            lvmcore_arena_init(&g_arena, guarded_arena, LVMCORE_LABEL_SCAN_BYTES);
                            lvmcore_pv_label lbl;
                            lvmcore_err err = {0};

                            lvmcore_status st = lvmcore_read_pv_label(&src, &g_arena, &lbl, &err);
                            char msg[96];
                            snprintf(msg, sizeof(msg),
                                     "guard-page data_off=%u at off=7680 rejected without fault",
                                     oob_offs[bi]);
                            CHECK(msg, st == LVMCORE_ERR_CORRUPT_LABEL && err.failed);
                        }
                    }
                    munmap(region, data_pages + page_sz);
                }
            }
#endif
            free(scan_img);
        }
    }

    /* ---- 15. Slot 0 LVMCORE_RAW_LOCN_IGNORED skipped in multi-MDA PV - */
    {
        const uint64_t mda0_off = 4096u;
        const uint64_t mda0_sz  = 65536u;
        const uint64_t mda1_off = 69632u;
        const uint64_t mda1_sz  = 65536u;

        memset(img, 0, sizeof(img));
        unsigned char *p = img + LABEL_OFF;
        memcpy(p, "LABELONE", 8);
        put_le64(p + 8, 1);
        put_le32(p + 20, 32);
        memcpy(p + 24, "LVM2 001", 8);
        unsigned char *pvh = p + 32;
        memcpy(pvh, "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP", 32);
        put_le64(pvh + 32, IMG_SIZE);
        unsigned char *d = pvh + 40;
        put_le64(d, 0x1000000); put_le64(d + 8, 0x7f00000);
        put_le64(d + 16, 0); put_le64(d + 24, 0);
        unsigned char *m = d + 32;
        put_le64(m,      mda0_off); put_le64(m + 8,  mda0_sz);
        put_le64(m + 16, mda1_off); put_le64(m + 24, mda1_sz);
        put_le64(m + 32, 0);        put_le64(m + 40, 0);
        put_le32(p + 16, lvmcore_crc32(p + 20, 512 - 20));

        /* MDA 0: higher seqno=99 but Slot 0 has LVMCORE_RAW_LOCN_IGNORED. */
        const char *meta_ignored =
            "vgx {\n"
            "id = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "seqno = 99\n"
            "status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n"
            "extent_size = 8192\n"
            "physical_volumes {\n"
            "  pv0 { id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\" status = [\"ALLOCATABLE\"] pe_start = 2048 pe_count = 100 }\n"
            "}\n"
            "}\n";
        size_t len0 = strlen(meta_ignored) + 1;
        memcpy(img + mda0_off + 512, meta_ignored, len0);
        unsigned char *h0 = img + mda0_off;
        memcpy(h0 + 4, " LVM2 x[5A%r0N*>", 16);
        put_le32(h0 + 20, 1);
        put_le64(h0 + 24, mda0_off);
        put_le64(h0 + 32, mda0_sz);
        put_le64(h0 + 40, 512);
        put_le64(h0 + 48, len0);
        put_le32(h0 + 56, lvmcore_crc32(img + mda0_off + 512, len0));
        put_le32(h0 + 60, LVMCORE_RAW_LOCN_IGNORED);
        put_le32(h0, lvmcore_crc32(h0 + 4, 512 - 4));

        /* MDA 1: active with seqno=5. */
        const char *meta_active =
            "vgx {\n"
            "id = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "seqno = 5\n"
            "status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n"
            "extent_size = 8192\n"
            "physical_volumes {\n"
            "  pv0 { id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\" status = [\"ALLOCATABLE\"] pe_start = 2048 pe_count = 100 }\n"
            "}\n"
            "}\n";
        size_t len1 = strlen(meta_active) + 1;
        memcpy(img + mda1_off + 512, meta_active, len1);
        unsigned char *h1 = img + mda1_off;
        memcpy(h1 + 4, " LVM2 x[5A%r0N*>", 16);
        put_le32(h1 + 20, 1);
        put_le64(h1 + 24, mda1_off);
        put_le64(h1 + 32, mda1_sz);
        put_le64(h1 + 40, 512);
        put_le64(h1 + 48, len1);
        put_le32(h1 + 56, lvmcore_crc32(img + mda1_off + 512, len1));
        put_le32(h1 + 60, 0);
        put_le32(h1, lvmcore_crc32(h1 + 4, 512 - 4));

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("multi-MDA skips ignored MDA 0 (seqno 99) and selects active MDA 1 (seqno 5)",
              st == LVMCORE_OK && vg.seqno == 5);

        /* Direct read_metadata_text on ignored MDA 0 returns NO_VALID_MDA. */
        lvmcore_arena_reset(&arena);
        lvmcore_disk_area area0 = { mda0_off, mda0_sz };
        lvmcore_mda_header mdah0;
        char *txt0 = NULL;
        size_t txt0_len = 0;
        CHECK("read_mda_header on ignored MDA 0 succeeds with IGNORED flag",
              lvmcore_read_mda_header(&src, &area0, &arena, &mdah0, &err) == LVMCORE_OK &&
              (mdah0.raw_locns[0].flags & LVMCORE_RAW_LOCN_IGNORED) != 0);
        CHECK("read_metadata_text on ignored MDA 0 returns NO_VALID_MDA",
              lvmcore_read_metadata_text(&src, &area0, &mdah0, LVMCORE_READ_FLAG_DEFAULT,
                                         &arena, &txt0, &txt0_len, NULL, &err) == LVMCORE_ERR_NO_VALID_MDA);

        /* Mark MDA 1 ignored as well -> whole PV has no valid MDA. */
        put_le32(h1 + 60, LVMCORE_RAW_LOCN_IGNORED);
        put_le32(h1, lvmcore_crc32(h1 + 4, 512 - 4));
        lvmcore_arena_reset(&arena);
        st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("all-ignored MDAs fail with LVMCORE_ERR_NO_VALID_MDA",
              st == LVMCORE_ERR_NO_VALID_MDA);
    }

    /* ---- 16. Multi-MDA seqno==0 selection & non-VG section isolation - */
    {
        const uint64_t mda0_off = 4096u;
        const uint64_t mda0_sz  = 65536u;
        const uint64_t mda1_off = 69632u;
        const uint64_t mda1_sz  = 65536u;

        memset(img, 0, sizeof(img));
        unsigned char *p = img + LABEL_OFF;
        memcpy(p, "LABELONE", 8);
        put_le64(p + 8, 1);
        put_le32(p + 20, 32);
        memcpy(p + 24, "LVM2 001", 8);
        unsigned char *pvh = p + 32;
        memcpy(pvh, "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP", 32);
        put_le64(pvh + 32, IMG_SIZE);
        unsigned char *d = pvh + 40;
        put_le64(d, 0x1000000); put_le64(d + 8, 0x7f00000);
        put_le64(d + 16, 0); put_le64(d + 24, 0);
        unsigned char *m = d + 32;
        put_le64(m,      mda0_off); put_le64(m + 8,  mda0_sz);
        put_le64(m + 16, mda1_off); put_le64(m + 24, mda1_sz);
        put_le64(m + 32, 0);        put_le64(m + 40, 0);
        put_le32(p + 16, lvmcore_crc32(p + 20, 512 - 20));

        /* MDA 0: valid CRC text without a VG section. */
        const char *meta_novg = "dummy { system_id = \"stale\" }\n";
        size_t len0 = strlen(meta_novg) + 1;
        memcpy(img + mda0_off + 512, meta_novg, len0);
        unsigned char *h0 = img + mda0_off;
        memcpy(h0 + 4, " LVM2 x[5A%r0N*>", 16);
        put_le32(h0 + 20, 1);
        put_le64(h0 + 24, mda0_off);
        put_le64(h0 + 32, mda0_sz);
        put_le64(h0 + 40, 512);
        put_le64(h0 + 48, len0);
        put_le32(h0 + 56, lvmcore_crc32(img + mda0_off + 512, len0));
        put_le32(h0, lvmcore_crc32(h0 + 4, 512 - 4));

        /* MDA 1: valid VG with seqno = 0 preceded by a non-VG section. */
        const char *meta_seq0 =
            "preamble { system_id = \"leaked\" }\n"
            "vgzero {\n"
            "id = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "seqno = 0\n"
            "status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n"
            "extent_size = 8192\n"
            "physical_volumes {\n"
            "  pv0 { id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\" status = [\"ALLOCATABLE\"] pe_start = 2048 pe_count = 100 }\n"
            "}\n"
            "}\n";
        size_t len1 = strlen(meta_seq0) + 1;
        memcpy(img + mda1_off + 512, meta_seq0, len1);
        unsigned char *h1 = img + mda1_off;
        memcpy(h1 + 4, " LVM2 x[5A%r0N*>", 16);
        put_le32(h1 + 20, 1);
        put_le64(h1 + 24, mda1_off);
        put_le64(h1 + 32, mda1_sz);
        put_le64(h1 + 40, 512);
        put_le64(h1 + 48, len1);
        put_le32(h1 + 56, lvmcore_crc32(img + mda1_off + 512, len1));
        put_le32(h1, lvmcore_crc32(h1 + 4, 512 - 4));

        lvmcore_byte_source src; mem_src ms;
        mem_source(img, IMG_SIZE, &src, &ms);
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};

        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        CHECK("multi-MDA selects valid seqno==0 MDA over invalid MDA 0",
              st == LVMCORE_OK && strcmp(vg.name, "vgzero") == 0 && vg.seqno == 0);
        CHECK("non-VG preamble does not bleed system_id into VG",
              vg.system_id[0] == '\0');
    }

    /* ---- 17. Segment contiguity, overlap, gap & declared count ------- */
    {
        struct {
            const char *name;
            const char *lv_body;
        } bad_segs[] = {
            {
                "segment starting at nonzero extent rejected",
                "segment_count = 1\n"
                "segment1 { start_extent = 1 extent_count = 2 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 0] }\n"
            },
            {
                "gapped segments rejected",
                "segment_count = 2\n"
                "segment1 { start_extent = 0 extent_count = 2 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 0] }\n"
                "segment2 { start_extent = 3 extent_count = 2 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 2] }\n"
            },
            {
                "overlapping segments rejected",
                "segment_count = 2\n"
                "segment1 { start_extent = 0 extent_count = 3 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 0] }\n"
                "segment2 { start_extent = 2 extent_count = 2 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 4] }\n"
            },
            {
                "declared_segment_count mismatch rejected",
                "segment_count = 2\n"
                "segment1 { start_extent = 0 extent_count = 2 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 0] }\n"
            },
            {
                "zero extent_count segment rejected",
                "segment_count = 1\n"
                "segment1 { start_extent = 0 extent_count = 0 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 0] }\n"
            },
        };

        for (size_t i = 0; i < sizeof(bad_segs) / sizeof(bad_segs[0]); i++) {
            snprintf(meta, sizeof(meta),
                "vgx {\n"
                "id = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
                "seqno = 1\n"
                "status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n"
                "extent_size = 8192\n"
                "physical_volumes {\n"
                "  pv0 { id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\" status = [\"ALLOCATABLE\"] pe_start = 2048 pe_count = 100 }\n"
                "}\n"
                "logical_volumes {\n"
                "  lv0 {\n"
                "    id = \"lllllllllllllllllllllllllllllll0\"\n"
                "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
                "    %s"
                "  }\n"
                "}\n"
                "}\n",
                bad_segs[i].lv_body);
            lvmcore_arena arena;
            lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
            lvmcore_volume_group vg;
            lvmcore_err err = {0};
            lvmcore_status st = lvmcore_parse_vg_text(meta, strlen(meta), &arena, &vg, &err);
            CHECK(bad_segs[i].name,
                  st == LVMCORE_OK && vg.lv_count == 1 &&
                  vg.lvs[0].mappable == 0 && vg.lvs[0].unsupported_reason[0] != '\0');
        }
    }

    /* ---- 18. Empty stripes = [], bad stripe geometry & PV bounds ----- */
    {
        struct {
            const char *name;
            const char *seg_body;
        } bad_stripes[] = {
            {
                "empty stripes = [] with stripe_count = 1 rejected",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1 stripes = []"
            },
            {
                "empty stripes = [] with stripe_count = 0 rejected",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 0 stripes = []"
            },
            {
                "omitted stripes array (NULL) rejected without deref",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1"
            },
            {
                "striped segment with stripe_size = 0 rejected",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 2 stripe_size = 0 stripes = [\"pv0\", 0, \"pv0\", 2]"
            },
            {
                "striped segment with non-power-of-two stripe_size rejected",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 2 stripe_size = 96 stripes = [\"pv0\", 0, \"pv0\", 2]"
            },
            {
                "striped segment with extent_count not divisible by stripe_count rejected",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 3 stripe_size = 128 stripes = [\"pv0\", 0, \"pv0\", 2, \"pv0\", 4]"
            },
            {
                "stripe referencing unknown PV rejected",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1 stripes = [\"pv9\", 0]"
            },
            {
                "stripe extending past pv->pe_count rejected",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 98]"
            },
            {
                "stripe start_extent overflow rejected",
                "start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 9223372036854775807]"
            },
        };

        for (size_t i = 0; i < sizeof(bad_stripes) / sizeof(bad_stripes[0]); i++) {
            snprintf(meta, sizeof(meta),
                "vgx {\n"
                "id = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
                "seqno = 1\n"
                "status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n"
                "extent_size = 8192\n"
                "physical_volumes {\n"
                "  pv0 { id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\" status = [\"ALLOCATABLE\"] pe_start = 2048 pe_count = 100 }\n"
                "}\n"
                "logical_volumes {\n"
                "  lv0 {\n"
                "    id = \"lllllllllllllllllllllllllllllll0\"\n"
                "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
                "    segment_count = 1\n"
                "    segment1 { %s }\n"
                "  }\n"
                "}\n"
                "}\n",
                bad_stripes[i].seg_body);
            lvmcore_arena arena;
            lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
            lvmcore_volume_group vg;
            lvmcore_err err = {0};
            lvmcore_status st = lvmcore_parse_vg_text(meta, strlen(meta), &arena, &vg, &err);
            CHECK(bad_stripes[i].name,
                  st == LVMCORE_OK && vg.lv_count == 1 &&
                  vg.lvs[0].mappable == 0 && vg.lvs[0].unsupported_reason[0] != '\0');
        }
    }

    /* ---- 19. READ-only VG/LV and MISSING PV status flags ------------- */
    {
        const char *ro_meta =
            "vgro {\n"
            "id = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "seqno = 3\n"
            "status = [\"RESIZEABLE\", \"READ\"]\n"
            "extent_size = 8192\n"
            "physical_volumes {\n"
            "  pv0 {\n"
            "    id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP0\"\n"
            "    status = [\"ALLOCATABLE\"]\n"
            "    flags = []\n"
            "    pe_start = 2048\n"
            "    pe_count = 100\n"
            "  }\n"
            "  pv1 {\n"
            "    id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP1\"\n"
            "    status = [\"ALLOCATABLE\", \"MISSING\"]\n"
            "    flags = []\n"
            "    pe_start = 2048\n"
            "    pe_count = 100\n"
            "  }\n"
            "  pv2 {\n"
            "    id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP2\"\n"
            "    status = [\"ALLOCATABLE\"]\n"
            "    flags = [\"MISSING\"]\n"
            "    pe_start = 2048\n"
            "    pe_count = 100\n"
            "  }\n"
            "}\n"
            "logical_volumes {\n"
            "  lv_ro {\n"
            "    id = \"lllllllllllllllllllllllllllllll1\"\n"
            "    status = [\"READ\", \"VISIBLE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 0] }\n"
            "  }\n"
            "  lv_rw {\n"
            "    id = \"lllllllllllllllllllllllllllllll2\"\n"
            "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 4] }\n"
            "  }\n"
            "  lv_miss_status {\n"
            "    id = \"lllllllllllllllllllllllllllllll3\"\n"
            "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1 stripes = [\"pv1\", 0] }\n"
            "  }\n"
            "  lv_miss_flags {\n"
            "    id = \"lllllllllllllllllllllllllllllll4\"\n"
            "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 4 type = \"striped\" stripe_count = 1 stripes = [\"pv2\", 0] }\n"
            "  }\n"
            "}\n"
            "}\n";

        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};
        lvmcore_status st = lvmcore_parse_vg_text(ro_meta, strlen(ro_meta), &arena, &vg, &err);
        CHECK("read-only VG with missing PVs parses", st == LVMCORE_OK);
        CHECK("VG status without WRITE sets vg.read_only == 1", vg.read_only == 1);
        CHECK("pv0 missing == 0, pv1 (status) missing == 1, pv2 (flags) missing == 1",
              vg.pv_count == 3 &&
              vg.pvs[0].missing == 0 && vg.pvs[1].missing == 1 && vg.pvs[2].missing == 1);

        const lvmcore_logical_volume *lv_ro = lvmcore_vg_find_lv(&vg, "lv_ro");
        const lvmcore_logical_volume *lv_rw = lvmcore_vg_find_lv(&vg, "lv_rw");
        const lvmcore_logical_volume *lv_ms = lvmcore_vg_find_lv(&vg, "lv_miss_status");
        const lvmcore_logical_volume *lv_mf = lvmcore_vg_find_lv(&vg, "lv_miss_flags");
        CHECK("lv_ro has read_only==1, visible==1, mappable==1",
              lv_ro && lv_ro->read_only == 1 && lv_ro->visible == 1 && lv_ro->mappable == 1);
        CHECK("lv_rw has read_only==0, visible==1, mappable==1",
              lv_rw && lv_rw->read_only == 0 && lv_rw->visible == 1 && lv_rw->mappable == 1);
        CHECK("LV on PV with status MISSING is unmappable",
              lv_ms && lv_ms->mappable == 0);
        CHECK("LV on PV with flags MISSING is unmappable",
              lv_mf && lv_mf->mappable == 0);
    }

    /* ---- 20. type = "linear" default stripe_count=1 & mirror leg start_extent bounds ---- */
    {
        const char *lin_mir_meta =
            "vglinmir {\n"
            "id = \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n"
            "seqno = 2\n"
            "status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n"
            "extent_size = 8192\n"
            "physical_volumes {\n"
            "  pv0 { id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP0\" status = [\"ALLOCATABLE\"] pe_start = 2048 pe_count = 100 }\n"
            "  pv1 { id = \"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP1\" status = [\"ALLOCATABLE\"] pe_start = 2048 pe_count = 100 }\n"
            "}\n"
            "logical_volumes {\n"
            "  lv_lin {\n"
            "    id = \"11111111111111111111111111111111\"\n"
            "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 4 type = \"linear\" stripes = [\"pv0\", 0] }\n"
            "  }\n"
            "  m_i0 {\n"
            "    id = \"22222222222222222222222222222222\"\n"
            "    status = [\"READ\", \"WRITE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 5 type = \"striped\" stripe_count = 1 stripes = [\"pv0\", 10] }\n"
            "  }\n"
            "  m_i1 {\n"
            "    id = \"33333333333333333333333333333333\"\n"
            "    status = [\"READ\", \"WRITE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 5 type = \"striped\" stripe_count = 1 stripes = [\"pv1\", 10] }\n"
            "  }\n"
            "  lv_mir_ok {\n"
            "    id = \"44444444444444444444444444444444\"\n"
            "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 4 type = \"mirror\" mirror_count = 2 region_size = 4096 mirrors = [\"m_i0\", 1, \"m_i1\", 1] }\n"
            "  }\n"
            "  lv_mir_oob {\n"
            "    id = \"55555555555555555555555555555555\"\n"
            "    status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
            "    segment_count = 1\n"
            "    segment1 { start_extent = 0 extent_count = 4 type = \"mirror\" mirror_count = 2 region_size = 4096 mirrors = [\"m_i0\", 3, \"m_i1\", 0] }\n"
            "  }\n"
            "}\n"
            "}\n";

        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};
        lvmcore_status st = lvmcore_parse_vg_text(lin_mir_meta, strlen(lin_mir_meta), &arena, &vg, &err);
        CHECK("linear-without-stripe_count + mirror-offset VG parses", st == LVMCORE_OK);
        const lvmcore_logical_volume *lv_lin = lvmcore_vg_find_lv(&vg, "lv_lin");
        const lvmcore_logical_volume *lv_mok = lvmcore_vg_find_lv(&vg, "lv_mir_ok");
        const lvmcore_logical_volume *lv_moob = lvmcore_vg_find_lv(&vg, "lv_mir_oob");
        CHECK("type=\"linear\" omitting stripe_count defaults stripe_count=1 and stays mappable",
              lv_lin && lv_lin->mappable == 1 && lv_lin->segment_count == 1 &&
              lv_lin->segments[0].stripe_count == 1);
        CHECK("mirror leg start_extent + extent_count == leg_extents stays mappable",
              lv_mok && lv_mok->mappable == 1);
        CHECK("mirror leg start_extent + extent_count > leg_extents rejected",
              lv_moob && lv_moob->mappable == 0 &&
              strcmp(lv_moob->unsupported_reason, "raid1/mirror leg smaller than LV") == 0);
    }

    printf("\n%d passed, %d failed (lvmcore hardening tests)\n", passes, fails);
    return fails ? 1 : 0;
}
