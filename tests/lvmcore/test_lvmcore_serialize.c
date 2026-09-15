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
    uint8_t buf[1024];
} mock_state_t;

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

    const char sample_payload[] = "sample_vg { seqno = 1 }\n";
    uint32_t payload_crc = lvmcore_crc32(sample_payload, sizeof(sample_payload));
    uint64_t payload_size = (uint64_t)sizeof(sample_payload);

    /* Step 1: Precommit */
    lvmcore_mda_header pre_mdah;
    lvmcore_status st = lvmcore_mda_prepare_precommit(&mdah, 512, payload_size, payload_crc, &pre_mdah);
    CHECK("mda_prepare_precommit OK", st == LVMCORE_OK);
    CHECK("precommit slot 0 remains untouched (offset 0)", pre_mdah.raw_locns[0].offset == 0);
    CHECK("precommit slot 1 has new offset 512", pre_mdah.raw_locns[1].offset == 512);
    CHECK("precommit slot 1 has new size", pre_mdah.raw_locns[1].size == payload_size);
    CHECK("precommit slot 1 has new checksum", pre_mdah.raw_locns[1].checksum == payload_crc);

    /* Step 2: Binary serialize precommit header to 512-byte sector */
    uint8_t sector[512];
    st = lvmcore_mda_header_write(&pre_mdah, sector, sizeof(sector));
    CHECK("mda_header_write precommit OK", st == LVMCORE_OK);

    /* Verify CRC at sector 0..3 */
    uint32_t stored_crc = (uint32_t)sector[0] | ((uint32_t)sector[1] << 8) |
                          ((uint32_t)sector[2] << 16) | ((uint32_t)sector[3] << 24);
    uint32_t calc_crc = lvmcore_crc32(sector + 4, sizeof(sector) - 4);
    CHECK("mda_header_write CRC matches computed", stored_crc == calc_crc);

    /* Step 3: Commit */
    lvmcore_mda_header commit_mdah;
    st = lvmcore_mda_prepare_commit(&pre_mdah, 512, payload_size, payload_crc, &commit_mdah);
    CHECK("mda_prepare_commit OK", st == LVMCORE_OK);
    CHECK("commit slot 0 has new offset 512", commit_mdah.raw_locns[0].offset == 512);
    CHECK("commit slot 0 has new size", commit_mdah.raw_locns[0].size == payload_size);
    CHECK("commit slot 0 has new checksum", commit_mdah.raw_locns[0].checksum == payload_crc);
    CHECK("commit slot 1 is cleared (offset 0)", commit_mdah.raw_locns[1].offset == 0);
    CHECK("commit slot 1 is cleared (size 0)", commit_mdah.raw_locns[1].size == 0);

    /* Step 4: Write committed sector and verify parse using lvmcore_read_mda_header */
    st = lvmcore_mda_header_write(&commit_mdah, sector, sizeof(sector));
    CHECK("mda_header_write commit OK", st == LVMCORE_OK);

    static mock_state_t mock_state;
    memset(&mock_state, 0, sizeof(mock_state));
    memcpy(mock_state.buf, sector, sizeof(sector));

    lvmcore_byte_source mock_src;
    memset(&mock_src, 0, sizeof(mock_src));
    mock_src.impl = &mock_state;
    mock_src.size = sizeof(mock_state.buf);
    mock_src.read = mock_read;

    lvmcore_arena a;
    lvmcore_arena_init(&a, arena_buf1, sizeof(arena_buf1));
    lvmcore_disk_area area = { .offset = 0, .size = commit_mdah.size };
    lvmcore_mda_header read_back;
    lvmcore_err err;
    st = lvmcore_read_mda_header(&mock_src, &area, &a, &read_back, &err);
    CHECK("read_back written committed header OK", st == LVMCORE_OK);
    CHECK("read_back slot 0 offset matches 512", read_back.raw_locns[0].offset == 512);
    CHECK("read_back slot 0 size matches payload_size", read_back.raw_locns[0].size == payload_size);
    CHECK("read_back slot 0 checksum matches payload_crc", read_back.raw_locns[0].checksum == payload_crc);
    CHECK("read_back slot 1 offset is 0", read_back.raw_locns[1].offset == 0);
}

/* ------------------------------------------------------------------ */
/* Main test runner                                                   */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    const char *corpus_images_dir = (argc > 1) ? argv[1] : "build/corpus-images";

    printf("=== test_lvmcore_serialize ===\n");
    test_synthetic_vg_roundtrip();
    test_ring_placement();
    test_two_slot_commit_and_sector();
    test_corpus_roundtrip(corpus_images_dir);

    printf("\nSummary: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
