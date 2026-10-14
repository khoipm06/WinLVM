/*
 * test_lvmcore_serialize.c — unit tests for lvmcore metadata serializer
 * and two-slot commit engine.
 *
 * Verifies:
 *   1. Synthetic volume group serialization and parsing match.
 *   2. Semantic round-trip on all corpus images:
 *      parse(disk) -> serialize(vg) -> parse(text) -> assert vg_serialized == vg_disk.
 *   3. MDA ring placement engine (_next_rlocn_offset analog) under initial,
 *      sequential, near-end wrap, wrapped-prior, and overflow conditions.
 *   4. Two-slot commit lifecycle (precommit in slot 1, commit in slot 0)
 *      and raw on-disk MDA header serialization with CRC32 verification.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#define WL_FSEEK64 _fseeki64
#define WL_FTELL64 _ftelli64
#else
#define WL_FSEEK64 fseeko
#define WL_FTELL64 ftello
#endif

#include "lvmcore/types.h"
#include "lvmcore/arena.h"
#include "lvmcore/parser.h"
#include "lvmcore/serialize.h"

#define ARENA_CAP (4u * 1024 * 1024)
static uint8_t arena_buf1[ARENA_CAP];
static uint8_t arena_buf2[ARENA_CAP];

static char text_buf[LVMCORE_MAX_METADATA_BYTES];

static int passes = 0;
static int fails = 0;

#define CHECK(name, cond) do { \
    if (cond) { passes++; printf("[  OK  ] %s\n", name); } \
    else { fails++; printf("[ FAIL ] %s\n", name); } } while (0)

static size_t file_read(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (WL_FSEEK64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

typedef struct {
    uint8_t buf[4096];
} mock_state_t;

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static size_t mock_read(void *impl, uint64_t offset, void *buf, size_t len) {
    mock_state_t *s = (mock_state_t *)impl;
    if (offset >= sizeof(s->buf) || len > sizeof(s->buf) - offset) return 0;
    memcpy(buf, s->buf + offset, len);
    return len;
}

/* ------------------------------------------------------------------ */
/* Test 1: Synthetic VG Serialization Round-Trip                      */
/* ------------------------------------------------------------------ */

static void test_synthetic_vg_roundtrip(void) {
    lvmcore_arena a1, a2;
    lvmcore_arena_init(&a1, arena_buf1, sizeof(arena_buf1));
    lvmcore_arena_init(&a2, arena_buf2, sizeof(arena_buf2));

    lvmcore_volume_group vg;
    memset(&vg, 0, sizeof(vg));
    snprintf(vg.name, sizeof(vg.name), "vgsynth");
    snprintf(vg.id, sizeof(vg.id), "11111122223333444455556666777788");
    vg.seqno = 42;
    vg.extent_size_sectors = 8192;
    vg.pv_count = 2;
    vg.pvs = (lvmcore_physical_volume *)lvmcore_arena_alloc_zero(&a1, 2 * sizeof(lvmcore_physical_volume), 8);

    snprintf(vg.pvs[0].key, sizeof(vg.pvs[0].key), "pv0");
    snprintf(vg.pvs[0].id, sizeof(vg.pvs[0].id), "aaaaaabbbbccccddddeeeeffff000011");
    snprintf(vg.pvs[0].device, sizeof(vg.pvs[0].device), "/dev/loop0");
    vg.pvs[0].dev_size_sectors = 262144;
    vg.pvs[0].pe_start = 2048;
    vg.pvs[0].pe_count = 31;

    snprintf(vg.pvs[1].key, sizeof(vg.pvs[1].key), "pv1");
    snprintf(vg.pvs[1].id, sizeof(vg.pvs[1].id), "bbbbbbccccddddeeeeffffaaaa111122");
    snprintf(vg.pvs[1].device, sizeof(vg.pvs[1].device), "/dev/loop1");
    vg.pvs[1].dev_size_sectors = 262144;
    vg.pvs[1].pe_start = 2048;
    vg.pvs[1].pe_count = 31;

    vg.lv_count = 1;
    vg.lvs = (lvmcore_logical_volume *)lvmcore_arena_alloc_zero(&a1, sizeof(lvmcore_logical_volume), 8);
    snprintf(vg.lvs[0].name, sizeof(vg.lvs[0].name), "lvsynth");
    snprintf(vg.lvs[0].id, sizeof(vg.lvs[0].id), "ccccccddddeeeeffffaaaabbbb222233");
    vg.lvs[0].visible = 1;
    vg.lvs[0].mappable = 1;
    vg.lvs[0].segment_count = 1;
    vg.lvs[0].segments = (lvmcore_segment *)lvmcore_arena_alloc_zero(&a1, sizeof(lvmcore_segment), 8);
    vg.lvs[0].segments[0].start_extent = 0;
    vg.lvs[0].segments[0].extent_count = 8;
    vg.lvs[0].segments[0].type = LVMCORE_SEG_STRIPED;
    snprintf(vg.lvs[0].segments[0].seg_type_name, sizeof(vg.lvs[0].segments[0].seg_type_name), "striped");
    vg.lvs[0].segments[0].stripe_count = 2;
    vg.lvs[0].segments[0].stripe_size_sectors = 128;
    vg.lvs[0].segments[0].stripe_count_used = 2;
    vg.lvs[0].segments[0].stripes = (lvmcore_stripe *)lvmcore_arena_alloc_zero(&a1, 2 * sizeof(lvmcore_stripe), 8);
    snprintf(vg.lvs[0].segments[0].stripes[0].pv_key, sizeof(vg.lvs[0].segments[0].stripes[0].pv_key), "pv0");
    vg.lvs[0].segments[0].stripes[0].start_extent = 0;
    snprintf(vg.lvs[0].segments[0].stripes[1].pv_key, sizeof(vg.lvs[0].segments[0].stripes[1].pv_key), "pv1");
    vg.lvs[0].segments[0].stripes[1].start_extent = 0;

    size_t out_len = 0;
    lvmcore_status st = lvmcore_serialize_vg(&vg, text_buf, sizeof(text_buf), &out_len);
    CHECK("synthetic vg serialize status OK", st == LVMCORE_OK);
    CHECK("synthetic vg serialize length > 0", out_len > 0);

    lvmcore_volume_group parsed_vg;
    lvmcore_err err;
    st = lvmcore_parse_vg_text(text_buf, out_len - 1, &a2, &parsed_vg, &err);
    CHECK("synthetic vg parse status OK", st == LVMCORE_OK);
    CHECK("synthetic vg name match", strcmp(vg.name, parsed_vg.name) == 0);
    CHECK("synthetic vg id match", strcmp(vg.id, parsed_vg.id) == 0);
    CHECK("synthetic vg seqno match", vg.seqno == parsed_vg.seqno);
    CHECK("synthetic vg extent_size match", vg.extent_size_sectors == parsed_vg.extent_size_sectors);
    CHECK("synthetic vg pv_count match", vg.pv_count == parsed_vg.pv_count);
    CHECK("synthetic vg lv_count match", vg.lv_count == parsed_vg.lv_count);
    CHECK("synthetic vg lv0 name match", strcmp(vg.lvs[0].name, parsed_vg.lvs[0].name) == 0);
    CHECK("synthetic vg lv0 segment_count match", parsed_vg.lvs[0].segment_count == 1);
    CHECK("synthetic vg lv0 stripe_count match", parsed_vg.lvs[0].segments[0].stripe_count == 2);
    CHECK("synthetic vg lv0 stripe_size match", parsed_vg.lvs[0].segments[0].stripe_size_sectors == 128);
}

/* ------------------------------------------------------------------ */
/* Test 2: Semantic Round-Trip on all Corpus Images                   */
/* ------------------------------------------------------------------ */

static const char *kCorpusImages[] = {
    "linear-1pv.img",
    "linear-1pv-multiseg.img",
    "linear-2pv-a.img",
    "linear-2pv-b.img",
    "striped-2pv-a.img",
    "striped-2pv-b.img",
    "sector4k.img",
    "odd-geometry.img",
    "mixed-vg-a.img",
    "mixed-vg-b.img"
};

static void test_corpus_roundtrip(const char *corpus_images_dir) {
    for (size_t i = 0; i < sizeof(kCorpusImages)/sizeof(kCorpusImages[0]); i++) {
        char img_path[512];
        snprintf(img_path, sizeof(img_path), "%s/%s", corpus_images_dir, kCorpusImages[i]);
        FILE *f = fopen(img_path, "rb");
        if (!f) {
            printf("[ SKIP ] %s (file not found)\n", img_path);
            continue;
        }
        WL_FSEEK64(f, 0, SEEK_END);
        uint64_t fsize = (uint64_t)WL_FTELL64(f);
        WL_FSEEK64(f, 0, SEEK_SET);

        lvmcore_byte_source src = {
            .impl = f,
            .size = fsize,
            .read = file_read
        };

        lvmcore_arena a_disk, a_round;
        lvmcore_arena_init(&a_disk, arena_buf1, sizeof(arena_buf1));
        lvmcore_arena_init(&a_round, arena_buf2, sizeof(arena_buf2));

        lvmcore_volume_group vg_disk;
        lvmcore_err err;
        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &a_disk, &vg_disk, &err);
        fclose(f);

        char title[128];
        snprintf(title, sizeof(title), "read_vg on %s", kCorpusImages[i]);
        CHECK(title, st == LVMCORE_OK);
        if (st != LVMCORE_OK) continue;

        /* Serialize the parsed VG to text */
        size_t ser_len = 0;
        st = lvmcore_serialize_vg(&vg_disk, text_buf, sizeof(text_buf), &ser_len);
        snprintf(title, sizeof(title), "serialize_vg on %s", kCorpusImages[i]);
        CHECK(title, st == LVMCORE_OK && ser_len > 0);
        if (st != LVMCORE_OK) continue;

        /* Parse back serialized text into a second VG */
        lvmcore_volume_group vg_round;
        st = lvmcore_parse_vg_text(text_buf, ser_len - 1, &a_round, &vg_round, &err);
        snprintf(title, sizeof(title), "parse_vg_text of serialized %s", kCorpusImages[i]);
        CHECK(title, st == LVMCORE_OK);
        if (st != LVMCORE_OK) continue;

        /* Compare semantic equivalence */
        snprintf(title, sizeof(title), "round-trip VG name for %s", kCorpusImages[i]);
        CHECK(title, strcmp(vg_disk.name, vg_round.name) == 0);

        snprintf(title, sizeof(title), "round-trip VG id for %s", kCorpusImages[i]);
        CHECK(title, strcmp(vg_disk.id, vg_round.id) == 0);

        snprintf(title, sizeof(title), "round-trip VG seqno for %s", kCorpusImages[i]);
        CHECK(title, vg_disk.seqno == vg_round.seqno);

        snprintf(title, sizeof(title), "round-trip VG extent_size for %s", kCorpusImages[i]);
        CHECK(title, vg_disk.extent_size_sectors == vg_round.extent_size_sectors);

        snprintf(title, sizeof(title), "round-trip VG pv_count for %s", kCorpusImages[i]);
        CHECK(title, vg_disk.pv_count == vg_round.pv_count);
        CHECK("round-trip VG read_only matches", vg_disk.read_only == vg_round.read_only);

        for (size_t p = 0; p < vg_disk.pv_count; p++) {
            const lvmcore_physical_volume *p1 = &vg_disk.pvs[p];
            const lvmcore_physical_volume *p2 = lvmcore_vg_find_pv(&vg_round, p1->key);
            snprintf(title, sizeof(title), "PV %s present in %s", p1->key, kCorpusImages[i]);
            CHECK(title, p2 != NULL);
            if (!p2) continue;
            CHECK("PV id matches", strcmp(p1->id, p2->id) == 0);
            CHECK("PV pe_start matches", p1->pe_start == p2->pe_start);
            CHECK("PV pe_count matches", p1->pe_count == p2->pe_count);
            CHECK("PV dev_size matches", p1->dev_size_sectors == p2->dev_size_sectors);
            CHECK("PV missing matches", p1->missing == p2->missing);
        }

        snprintf(title, sizeof(title), "round-trip VG lv_count for %s", kCorpusImages[i]);
        CHECK(title, vg_disk.lv_count == vg_round.lv_count);

        for (size_t l = 0; l < vg_disk.lv_count; l++) {
            const lvmcore_logical_volume *lv1 = &vg_disk.lvs[l];
            const lvmcore_logical_volume *lv2 = lvmcore_vg_find_lv(&vg_round, lv1->name);
            snprintf(title, sizeof(title), "LV %s present in %s", lv1->name, kCorpusImages[i]);
            CHECK(title, lv2 != NULL);
            if (!lv2) continue;

            CHECK("LV id matches", strcmp(lv1->id, lv2->id) == 0);
            CHECK("LV visible matches", lv1->visible == lv2->visible);
            CHECK("LV read_only matches", lv1->read_only == lv2->read_only);
            CHECK("LV mappable matches", lv1->mappable == lv2->mappable);
            CHECK("LV segment_count matches", lv1->segment_count == lv2->segment_count);
            for (size_t s = 0; s < lv1->segment_count; s++) {
                const lvmcore_segment *s1 = &lv1->segments[s];
                const lvmcore_segment *s2 = &lv2->segments[s];
                CHECK("segment start_extent matches", s1->start_extent == s2->start_extent);
                CHECK("segment extent_count matches", s1->extent_count == s2->extent_count);
                CHECK("segment stripe_count matches", s1->stripe_count == s2->stripe_count);
                CHECK("segment stripe_count_used matches", s1->stripe_count_used == s2->stripe_count_used);
                for (size_t st_idx = 0; st_idx < s1->stripe_count_used; st_idx++) {
                    CHECK("stripe pv_key matches", strcmp(s1->stripes[st_idx].pv_key, s2->stripes[st_idx].pv_key) == 0);
                    CHECK("stripe start_extent matches", s1->stripes[st_idx].start_extent == s2->stripes[st_idx].start_extent);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Test 2b: READ-Only VG/LV, MISSING PV, and Escaping Round-Trip      */
/* ------------------------------------------------------------------ */

static void test_status_flags_roundtrip(void) {
    lvmcore_arena a1, a2;
    lvmcore_arena_init(&a1, arena_buf1, sizeof(arena_buf1));
    lvmcore_arena_init(&a2, arena_buf2, sizeof(arena_buf2));

    lvmcore_volume_group vg;
    memset(&vg, 0, sizeof(vg));
    snprintf(vg.name, sizeof(vg.name), "vgro");
    snprintf(vg.id, sizeof(vg.id), "11111122223333444455556666777799");
    snprintf(vg.system_id, sizeof(vg.system_id), "foreign-host");
    vg.seqno = 7;
    vg.extent_size_sectors = 8192;
    vg.read_only = 1;
    vg.pv_count = 2;
    vg.pvs = (lvmcore_physical_volume *)lvmcore_arena_alloc_zero(&a1, 2 * sizeof(lvmcore_physical_volume), 8);

    snprintf(vg.pvs[0].key, sizeof(vg.pvs[0].key), "pv0");
    snprintf(vg.pvs[0].id, sizeof(vg.pvs[0].id), "aaaaaabbbbccccddddeeeeffff000011");
    snprintf(vg.pvs[0].device, sizeof(vg.pvs[0].device), "\\\\.\\PhysicalDrive1");
    vg.pvs[0].dev_size_sectors = 262144;
    vg.pvs[0].pe_start = 2048;
    vg.pvs[0].pe_count = 31;
    vg.pvs[0].missing = 0;

    snprintf(vg.pvs[1].key, sizeof(vg.pvs[1].key), "pv1");
    snprintf(vg.pvs[1].id, sizeof(vg.pvs[1].id), "bbbbbbccccddddeeeeffffaaaa111122");
    vg.pvs[1].dev_size_sectors = 262144;
    vg.pvs[1].pe_start = 2048;
    vg.pvs[1].pe_count = 31;
    vg.pvs[1].missing = 1;

    vg.lv_count = 3;
    vg.lvs = (lvmcore_logical_volume *)lvmcore_arena_alloc_zero(&a1, 3 * sizeof(lvmcore_logical_volume), 8);

    /* lv_ro: visible, read-only, on healthy pv0 */
    snprintf(vg.lvs[0].name, sizeof(vg.lvs[0].name), "lv_ro");
    snprintf(vg.lvs[0].id, sizeof(vg.lvs[0].id), "ccccccddddeeeeffffaaaabbbb222201");
    vg.lvs[0].visible = 1;
    vg.lvs[0].read_only = 1;
    vg.lvs[0].segment_count = 1;
    vg.lvs[0].segments = (lvmcore_segment *)lvmcore_arena_alloc_zero(&a1, sizeof(lvmcore_segment), 8);
    vg.lvs[0].segments[0].start_extent = 0;
    vg.lvs[0].segments[0].extent_count = 4;
    vg.lvs[0].segments[0].type = LVMCORE_SEG_STRIPED;
    snprintf(vg.lvs[0].segments[0].seg_type_name, sizeof(vg.lvs[0].segments[0].seg_type_name), "striped");
    vg.lvs[0].segments[0].stripe_count = 1;
    vg.lvs[0].segments[0].stripe_count_used = 1;
    vg.lvs[0].segments[0].stripes = (lvmcore_stripe *)lvmcore_arena_alloc_zero(&a1, sizeof(lvmcore_stripe), 8);
    snprintf(vg.lvs[0].segments[0].stripes[0].pv_key, sizeof(vg.lvs[0].segments[0].stripes[0].pv_key), "pv0");

    /* lv_hidden_ro: hidden, read-only, on healthy pv0 */
    snprintf(vg.lvs[1].name, sizeof(vg.lvs[1].name), "lv_hidden_ro");
    snprintf(vg.lvs[1].id, sizeof(vg.lvs[1].id), "ccccccddddeeeeffffaaaabbbb222202");
    vg.lvs[1].visible = 0;
    vg.lvs[1].read_only = 1;
    vg.lvs[1].segment_count = 1;
    vg.lvs[1].segments = (lvmcore_segment *)lvmcore_arena_alloc_zero(&a1, sizeof(lvmcore_segment), 8);
    vg.lvs[1].segments[0].start_extent = 0;
    vg.lvs[1].segments[0].extent_count = 4;
    vg.lvs[1].segments[0].type = LVMCORE_SEG_STRIPED;
    snprintf(vg.lvs[1].segments[0].seg_type_name, sizeof(vg.lvs[1].segments[0].seg_type_name), "striped");
    vg.lvs[1].segments[0].stripe_count = 1;
    vg.lvs[1].segments[0].stripe_count_used = 1;
    vg.lvs[1].segments[0].stripes = (lvmcore_stripe *)lvmcore_arena_alloc_zero(&a1, sizeof(lvmcore_stripe), 8);
    snprintf(vg.lvs[1].segments[0].stripes[0].pv_key, sizeof(vg.lvs[1].segments[0].stripes[0].pv_key), "pv0");
    vg.lvs[1].segments[0].stripes[0].start_extent = 4;

    /* lv_missing: visible, writable, on missing pv1 */
    snprintf(vg.lvs[2].name, sizeof(vg.lvs[2].name), "lv_missing");
    snprintf(vg.lvs[2].id, sizeof(vg.lvs[2].id), "ccccccddddeeeeffffaaaabbbb222203");
    vg.lvs[2].visible = 1;
    vg.lvs[2].read_only = 0;
    vg.lvs[2].segment_count = 1;
    vg.lvs[2].segments = (lvmcore_segment *)lvmcore_arena_alloc_zero(&a1, sizeof(lvmcore_segment), 8);
    vg.lvs[2].segments[0].start_extent = 0;
    vg.lvs[2].segments[0].extent_count = 4;
    vg.lvs[2].segments[0].type = LVMCORE_SEG_STRIPED;
    snprintf(vg.lvs[2].segments[0].seg_type_name, sizeof(vg.lvs[2].segments[0].seg_type_name), "striped");
    vg.lvs[2].segments[0].stripe_count = 1;
    vg.lvs[2].segments[0].stripe_count_used = 1;
    vg.lvs[2].segments[0].stripes = (lvmcore_stripe *)lvmcore_arena_alloc_zero(&a1, sizeof(lvmcore_stripe), 8);
    snprintf(vg.lvs[2].segments[0].stripes[0].pv_key, sizeof(vg.lvs[2].segments[0].stripes[0].pv_key), "pv1");

    size_t out_len = 0;
    lvmcore_status st = lvmcore_serialize_vg(&vg, text_buf, sizeof(text_buf), &out_len);
    CHECK("status flags VG serialize OK", st == LVMCORE_OK && out_len > 0);
    CHECK("serialized VG emits READ-only status",
          strstr(text_buf, "\tstatus = [\"RESIZEABLE\", \"READ\"]\n") != NULL);
    CHECK("serialized PV emits MISSING status",
          strstr(text_buf, "status = [\"ALLOCATABLE\", \"MISSING\"]") != NULL);
    CHECK("serialized Win32 device escapes backslashes",
          strstr(text_buf, "device = \"\\\\\\\\.\\\\PhysicalDrive1\"") != NULL);

    lvmcore_volume_group parsed;
    lvmcore_err err;
    st = lvmcore_parse_vg_text(text_buf, out_len - 1, &a2, &parsed, &err);
    CHECK("status flags VG re-parse OK", st == LVMCORE_OK);
    CHECK("round-trip VG read_only == 1", parsed.read_only == 1);
    CHECK("round-trip VG system_id preserved", strcmp(parsed.system_id, "foreign-host") == 0);
    CHECK("round-trip pv0 device unescaped", strcmp(parsed.pvs[0].device, "\\\\.\\PhysicalDrive1") == 0);
    CHECK("round-trip pv0 missing == 0, pv1 missing == 1",
          parsed.pv_count == 2 && parsed.pvs[0].missing == 0 && parsed.pvs[1].missing == 1);

    const lvmcore_logical_volume *p_ro = lvmcore_vg_find_lv(&parsed, "lv_ro");
    const lvmcore_logical_volume *p_hro = lvmcore_vg_find_lv(&parsed, "lv_hidden_ro");
    const lvmcore_logical_volume *p_miss = lvmcore_vg_find_lv(&parsed, "lv_missing");
    CHECK("round-trip lv_ro visible=1 read_only=1 mappable=1",
          p_ro && p_ro->visible == 1 && p_ro->read_only == 1 && p_ro->mappable == 1);
    CHECK("round-trip lv_hidden_ro visible=0 read_only=1 mappable=0",
          p_hro && p_hro->visible == 0 && p_hro->read_only == 1 && p_hro->mappable == 0);
    CHECK("round-trip lv_missing read_only=0 mappable=0 (PV missing)",
          p_miss && p_miss->visible == 1 && p_miss->read_only == 0 && p_miss->mappable == 0);
}

/* ------------------------------------------------------------------ */
/* Test 3: Ring Placement Engine Tests                                */
/* ------------------------------------------------------------------ */

static void test_ring_placement(void) {
    uint64_t mda_size = 1048576; /* 1 MiB */
    uint64_t offset = 0;
    lvmcore_status st;

    /* 1. NULL mdah (first allocation) */
    st = lvmcore_mda_next_offset(NULL, mda_size, 4096, 512, &offset);
    CHECK("ring initial offset == 512", st == LVMCORE_OK && offset == 512);

    /* 2. Empty/ignored mdah (slot 0 has 0 size) */
    lvmcore_mda_header mdah;
    memset(&mdah, 0, sizeof(mdah));
    mdah.size = mda_size;
    st = lvmcore_mda_next_offset(&mdah, mda_size, 4096, 512, &offset);
    CHECK("ring empty mdah offset == 512", st == LVMCORE_OK && offset == 512);

    /* 3. Slot 0 at 512 with size 1000 (old_last = 1511).
     * next_start = 1512. Rounded to 512 is 1536. */
    mdah.raw_locns[0].offset = 512;
    mdah.raw_locns[0].size = 1000;
    st = lvmcore_mda_next_offset(&mdah, mda_size, 4096, 512, &offset);
    CHECK("ring sequential offset == 1536", st == LVMCORE_OK && offset == 1536);

    /* 4. Near end of MDA (less than alignment bytes remaining) -> wraps to 512 */
    uint64_t small_mda = 10000;
    mdah.size = small_mda;
    mdah.raw_locns[0].offset = 8000;
    mdah.raw_locns[0].size = 1600; /* old_last = 9599. small_mda - 9599 = 401 < 512 */
    st = lvmcore_mda_next_offset(&mdah, small_mda, 1000, 512, &offset);
    CHECK("ring wrap near end == 512", st == LVMCORE_OK && offset == 512);

    /* 5. Rounded start exceeds mda_size - alignment -> wraps to 512 */
    mdah.raw_locns[0].offset = 8000;
    mdah.raw_locns[0].size = 1450; /* old_last = 9449. next_start = 9450 -> align 512 = 9728 > 10000 - 512 */
    st = lvmcore_mda_next_offset(&mdah, small_mda, 1000, 512, &offset);
    CHECK("ring wrap rounded start == 512", st == LVMCORE_OK && offset == 512);

    /* 6. Previous metadata was wrapped around:
     * offset = 9000, size = 1512. offset + size = 10512 > 10000.
     * wrap = 512. old_last = 512 + 512 - 1 = 1023.
     * next_start = 1024 (already 512-aligned). */
    mdah.raw_locns[0].offset = 9000;
    mdah.raw_locns[0].size = 1512;
    st = lvmcore_mda_next_offset(&mdah, small_mda, 1000, 512, &offset);
    CHECK("ring after wrapped commit == 1024", st == LVMCORE_OK && offset == 1024);

    /* 6b. Wrapped commit with wrap > 512 bytes:
     * offset = 9000, size = 2000 -> wrap = 1000, old_last = 1511 -> next = 1536. */
    mdah.raw_locns[0].offset = 9000;
    mdah.raw_locns[0].size = 2000;
    st = lvmcore_mda_next_offset(&mdah, small_mda, 1000, 512, &offset);
    CHECK("ring after >512B wrapped commit == 1536", st == LVMCORE_OK && offset == 1536);

    /* 7. Oversized payload -> overflow */
    st = lvmcore_mda_next_offset(NULL, mda_size, mda_size, 512, &offset);
    CHECK("ring oversized payload rejects OVERFLOW", st == LVMCORE_ERR_OVERFLOW);
}

/* ------------------------------------------------------------------ */
/* Test 4: Two-Slot Commit Lifecycle and MDA Sector Serialization     */
/* ------------------------------------------------------------------ */

static void test_two_slot_commit_and_sector(void) {
    lvmcore_mda_header mdah;
    memset(&mdah, 0, sizeof(mdah));
    mdah.start = 0;
    mdah.size = sizeof(mock_state_t);
    mdah.version = 1;

    const char payload_v1[] = "sample_vg { seqno = 1 }\n";
    uint32_t crc_v1 = lvmcore_crc32(payload_v1, sizeof(payload_v1));
    uint64_t size_v1 = (uint64_t)sizeof(payload_v1);

    const char payload_v2[] = "sample_vg { seqno = 2 }\n";
    uint32_t crc_v2 = lvmcore_crc32(payload_v2, sizeof(payload_v2));
    uint64_t size_v2 = (uint64_t)sizeof(payload_v2);

    /* Establish committed Slot 0 = v1 at offset 512. */
    lvmcore_mda_header cur_mdah;
    lvmcore_status st = lvmcore_mda_prepare_commit(&mdah, 512, size_v1, crc_v1, &cur_mdah);
    CHECK("initial commit v1 OK", st == LVMCORE_OK);

    /* Step 1: Precommit v2 in Slot 1 at offset 1024 while Slot 0 holds v1. */
    lvmcore_mda_header pre_mdah;
    st = lvmcore_mda_prepare_precommit(&cur_mdah, 1024, size_v2, crc_v2, &pre_mdah);
    CHECK("mda_prepare_precommit OK", st == LVMCORE_OK);
    CHECK("precommit slot 0 preserved (offset 512)", pre_mdah.raw_locns[0].offset == 512);
    CHECK("precommit slot 1 has new offset 1024", pre_mdah.raw_locns[1].offset == 1024);
    CHECK("precommit slot 1 has new size", pre_mdah.raw_locns[1].size == size_v2);
    CHECK("precommit slot 1 has new checksum", pre_mdah.raw_locns[1].checksum == crc_v2);

    /* Step 2: Binary serialize precommit header and assert exact 24B raw_locn layout. */
    uint8_t sector[512];
    st = lvmcore_mda_header_write(&pre_mdah, sector, sizeof(sector));
    CHECK("mda_header_write precommit OK", st == LVMCORE_OK);

    uint32_t stored_crc = get_le32(sector);
    uint32_t calc_crc = lvmcore_crc32(sector + 4, sizeof(sector) - 4);
    CHECK("mda_header_write CRC matches computed", stored_crc == calc_crc);

    /* Slot 0 at bytes 40..63; Slot 1 at bytes 64..87; bytes 88..135 zero. */
    CHECK("raw sector slot 0 at 40..63",
          get_le64(sector + 40) == 512 &&
          get_le64(sector + 48) == size_v1 &&
          get_le32(sector + 56) == crc_v1 &&
          get_le32(sector + 60) == 0);
    CHECK("raw sector slot 1 at 64..87",
          get_le64(sector + 64) == 1024 &&
          get_le64(sector + 72) == size_v2 &&
          get_le32(sector + 80) == crc_v2 &&
          get_le32(sector + 84) == 0);
    int tail_zero = 1;
    for (size_t b = 88; b < 136; b++) {
        if (sector[b] != 0) { tail_zero = 0; break; }
    }
    CHECK("raw sector bytes 88..135 are zero (no stale slot-2 write)", tail_zero == 1);

    /* Populate mock ring buffer with precommit header + v1 at 512 + v2 at 1024. */
    static mock_state_t mock_state;
    memset(&mock_state, 0, sizeof(mock_state));
    memcpy(mock_state.buf, sector, sizeof(sector));
    memcpy(mock_state.buf + 512, payload_v1, sizeof(payload_v1));
    memcpy(mock_state.buf + 1024, payload_v2, sizeof(payload_v2));

    lvmcore_byte_source mock_src;
    memset(&mock_src, 0, sizeof(mock_src));
    mock_src.impl = &mock_state;
    mock_src.size = sizeof(mock_state.buf);
    mock_src.read = mock_read;

    lvmcore_arena a;
    lvmcore_arena_init(&a, arena_buf1, sizeof(arena_buf1));
    lvmcore_disk_area area = { .offset = 0, .size = pre_mdah.size };
    lvmcore_mda_header read_pre;
    lvmcore_err err;
    st = lvmcore_read_mda_header(&mock_src, &area, &a, &read_pre, &err);
    CHECK("read_back precommit header OK", st == LVMCORE_OK);
    CHECK("read_back precommit slot 0 matches v1",
          read_pre.raw_locns[0].offset == 512 &&
          read_pre.raw_locns[0].size == size_v1 &&
          read_pre.raw_locns[0].checksum == crc_v1);
    CHECK("read_back precommit slot 1 matches v2",
          read_pre.raw_locns[1].offset == 1024 &&
          read_pre.raw_locns[1].size == size_v2 &&
          read_pre.raw_locns[1].checksum == crc_v2 &&
          read_pre.raw_locns[1].flags == 0);

    char *txt = NULL;
    size_t txt_len = 0;
    bool is_pre = false;
    st = lvmcore_read_metadata_text(&mock_src, &area, &read_pre, LVMCORE_READ_FLAG_DEFAULT,
                                    &a, &txt, &txt_len, &is_pre, &err);
    CHECK("DEFAULT read returns committed v1",
          st == LVMCORE_OK && !is_pre && strcmp(txt, payload_v1) == 0);

    st = lvmcore_read_metadata_text(&mock_src, &area, &read_pre, LVMCORE_READ_FLAG_PRECOMMITTED,
                                    &a, &txt, &txt_len, &is_pre, &err);
    CHECK("PRECOMMITTED read returns slot 1 v2",
          st == LVMCORE_OK && is_pre && strcmp(txt, payload_v2) == 0);

    /* Verify non-zero slot 1 flags serialize at byte 84..87 and parse back. */
    lvmcore_mda_header flag_mdah = pre_mdah;
    flag_mdah.raw_locns[1].flags = LVMCORE_RAW_LOCN_IGNORED;
    st = lvmcore_mda_header_write(&flag_mdah, sector, sizeof(sector));
    CHECK("slot 1 flags written at offset 84..87 (not 88)",
          st == LVMCORE_OK &&
          get_le32(sector + 84) == LVMCORE_RAW_LOCN_IGNORED &&
          get_le32(sector + 88) == 0);
    memcpy(mock_state.buf, sector, sizeof(sector));
    lvmcore_arena_reset(&a);
    st = lvmcore_read_mda_header(&mock_src, &area, &a, &read_pre, &err);
    CHECK("slot 1 flags parsed back from offset 84..87",
          st == LVMCORE_OK && read_pre.raw_locns[1].flags == LVMCORE_RAW_LOCN_IGNORED);
    /* With slot 1 marked ignored, PRECOMMITTED falls back to slot 0 v1. */
    st = lvmcore_read_metadata_text(&mock_src, &area, &read_pre, LVMCORE_READ_FLAG_PRECOMMITTED,
                                    &a, &txt, &txt_len, &is_pre, &err);
    CHECK("PRECOMMITTED ignores slot 1 when RAW_LOCN_IGNORED is set",
          st == LVMCORE_OK && !is_pre && strcmp(txt, payload_v1) == 0);

    /* Step 3: Commit v2 to Slot 0 and clear Slot 1. */
    lvmcore_mda_header commit_mdah;
    st = lvmcore_mda_prepare_commit(&pre_mdah, 1024, size_v2, crc_v2, &commit_mdah);
    CHECK("mda_prepare_commit OK", st == LVMCORE_OK);
    CHECK("commit slot 0 has new offset 1024", commit_mdah.raw_locns[0].offset == 1024);
    CHECK("commit slot 0 has new size", commit_mdah.raw_locns[0].size == size_v2);
    CHECK("commit slot 0 has new checksum", commit_mdah.raw_locns[0].checksum == crc_v2);
    CHECK("commit slot 1 is cleared (offset 0)", commit_mdah.raw_locns[1].offset == 0);
    CHECK("commit slot 1 is cleared (size 0)", commit_mdah.raw_locns[1].size == 0);

    /* Step 4: Write committed sector and verify round-trip read of v2. */
    st = lvmcore_mda_header_write(&commit_mdah, sector, sizeof(sector));
    CHECK("mda_header_write commit OK", st == LVMCORE_OK);
    int slot1_zero = 1;
    for (size_t b = 64; b < 88; b++) {
        if (sector[b] != 0) { slot1_zero = 0; break; }
    }
    CHECK("committed sector zeroes slot 1 bytes 64..87", slot1_zero == 1);

    memcpy(mock_state.buf, sector, sizeof(sector));
    lvmcore_arena_reset(&a);
    lvmcore_mda_header read_back;
    st = lvmcore_read_mda_header(&mock_src, &area, &a, &read_back, &err);
    CHECK("read_back written committed header OK", st == LVMCORE_OK);
    CHECK("read_back slot 0 offset matches 1024", read_back.raw_locns[0].offset == 1024);
    CHECK("read_back slot 0 size matches size_v2", read_back.raw_locns[0].size == size_v2);
    CHECK("read_back slot 0 checksum matches crc_v2", read_back.raw_locns[0].checksum == crc_v2);
    CHECK("read_back slot 1 offset is 0", read_back.raw_locns[1].offset == 0);

    st = lvmcore_read_metadata_text(&mock_src, &area, &read_back, LVMCORE_READ_FLAG_DEFAULT,
                                    &a, &txt, &txt_len, &is_pre, &err);
    CHECK("DEFAULT read after commit returns v2",
          st == LVMCORE_OK && !is_pre && strcmp(txt, payload_v2) == 0);
}

/* ------------------------------------------------------------------ */
/* Main test runner                                                   */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    const char *corpus_images_dir = (argc > 1) ? argv[1] : "build/corpus-images";

    printf("=== test_lvmcore_serialize ===\n");
    test_synthetic_vg_roundtrip();
    test_status_flags_roundtrip();
    test_ring_placement();
    test_two_slot_commit_and_sector();
    test_corpus_roundtrip(corpus_images_dir);

    printf("\nSummary: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
