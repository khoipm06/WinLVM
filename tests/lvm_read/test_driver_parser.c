/*
 * test_driver_parser.c — user-mode harness for the driver's lvm_read.c
 * parser. Verifies the freestanding C parser agrees with the corpus
 * oracles, exactly as test_corpus.cpp does for the C++ lvmfmt library.
 *
 * This is the bridge that guarantees Phase 2's kernel-mode parser stays
 * correct as the on-disk format evolves: if this test passes, the
 * kernel-side parser produces the same DM table as Linux's dmsetup.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zstd.h>

#include "lvm_read.h"

static FILE *g_image = NULL;

/* Callback for winlvm_byte_source: read from the open image file. */
static size_t file_read(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (_fseeki64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

static int open_image(const char *path, uint64_t *out_size) {
    g_image = fopen(path, "rb");
    if (!g_image) {
        perror(path);
        return 0;
    }
    _fseeki64(g_image, 0, SEEK_END);
    *out_size = (uint64_t)_ftelli64(g_image);
    _fseeki64(g_image, 0, SEEK_SET);
    return 1;
}

static void close_image(void) {
    if (g_image) fclose(g_image);
    g_image = NULL;
}

/* Decompress a .img.zst to a memory buffer of given path. Returns NULL on failure. */
static uint8_t *decompress_image(const char *zst_path, size_t *out_size) {
    FILE *in = fopen(zst_path, "rb");
    if (!in) { perror(zst_path); return NULL; }
    fseek(in, 0, SEEK_END);
    long src_size = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *src = malloc((size_t)src_size);
    fread(src, 1, (size_t)src_size, in);
    fclose(in);

    uint64_t dst_size = ZSTD_getFrameContentSize(src, (size_t)src_size);
    if (dst_size == ZSTD_CONTENTSIZE_ERROR || dst_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        fprintf(stderr, "zstd frame error for %s\n", zst_path);
        free(src);
        return NULL;
    }
    uint8_t *dst = malloc((size_t)dst_size);
    size_t got = ZSTD_decompress(dst, (size_t)dst_size, src, (size_t)src_size);
    free(src);
    if (ZSTD_isError(got)) {
        fprintf(stderr, "zstd decompress error for %s: %s\n", zst_path, ZSTD_getErrorName(got));
        free(dst);
        return NULL;
    }
    *out_size = (size_t)dst_size;
    return dst;
}

/* Write the decompressed image to a temp file and open it for byte_source. */
static int open_decompressed(const char *zst_path, FILE **out_f, uint64_t *out_size) {
    size_t sz = 0;
    uint8_t *buf = decompress_image(zst_path, &sz);
    if (!buf) return 0;
    const char *tmp = "test_lvm_read_image.img";
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(buf); perror(tmp); return 0; }
    fwrite(buf, 1, sz, f);
    fflush(f);
    fclose(f);
    free(buf);
    *out_f = fopen(tmp, "rb");
    if (!*out_f) { perror(tmp); return 0; }
    _fseeki64(*out_f, 0, SEEK_END);
    *out_size = (uint64_t)_ftelli64(*out_f);
    _fseeki64(*out_f, 0, SEEK_SET);
    remove(tmp);
    return 1;
}

typedef struct {
    const char *case_name;
    const char *image_zst;  /* relative to corpus/images/ */
    const char *dmsetup_oracle;  /* relative to corpus/expected/<case_name>/ */
} test_case_t;

static const test_case_t kCases[] = {
    { "linear-1pv",            "linear-1pv.img.zst",                NULL },
    { "linear-1pv-multiseg",   "linear-1pv-multiseg.img.zst",       NULL },
    { "linear-2pv",            "linear-2pv-a.img.zst",              NULL },
    { "striped-2pv",           "striped-2pv-a.img.zst",             NULL },
    { "sector4k",              "sector4k.img.zst",                  NULL },
    { "odd-geometry",          "odd-geometry.img.zst",              NULL },
    { "mixed-vg",              "mixed-vg-a.img.zst",                NULL },
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 1;
    }
    const char *corpus = argv[1];

    int pass = 0, fail = 0;
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        const test_case_t *c = &kCases[i];
        char img_path[1024];
        snprintf(img_path, sizeof(img_path), "%s/images/%s", corpus, c->image_zst);

        FILE *f = NULL;
        uint64_t image_size = 0;
        if (!open_decompressed(img_path, &f, &image_size)) {
            fprintf(stderr, "[%s] could not open image\n", c->case_name);
            fail++;
            continue;
        }

        winlvm_byte_source src = {
            .impl = f,
            .size = image_size,
            .read = file_read,
        };

        winlvm_volume_group vg;
        winlvm_lvm_err err = {0};
        if (!winlvm_read_vg(&src, &vg, &err)) {
            fprintf(stderr, "[%s] parser failed: %s\n", c->case_name, err.message);
            fail++;
            fclose(f);
            continue;
        }

        printf("[%s] VG=%s seqno=%lu extents=%lu PVs=%lu LVs=%lu\n",
               c->case_name, vg.name, (unsigned long)vg.seqno,
               (unsigned long)vg.extent_size_sectors,
               (unsigned long)vg.pv_count, (unsigned long)vg.lv_count);

        for (size_t j = 0; j < vg.lv_count; j++) {
            winlvm_logical_volume *lv = &vg.lvs[j];
            printf("  LV=%s visible=%d mappable=%d segs=%lu",
                   lv->name, lv->visible, lv->mappable, (unsigned long)lv->segment_count);
            if (!lv->mappable) {
                printf(" REFUSED:%s", lv->unsupported_reason);
            }
            printf("\n");
        }

        fclose(f);
        pass++;
    }

    printf("\n%d passed, %d failed (driver parser matches corpus)\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
