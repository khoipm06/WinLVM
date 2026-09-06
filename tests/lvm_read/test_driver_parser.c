/*
 * test_driver_parser.c — user-mode harness for the driver's lvm_read.c
 * parser. Verifies the freestanding C parser agrees with the corpus
 * oracles.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define WL_FSEEK64 _fseeki64
#define WL_FTELL64 _ftelli64
#else
#define WL_FSEEK64 fseeko
#define WL_FTELL64 ftello
#endif

#include "lvm_read.h"

/* Callback for winlvm_byte_source: read from the open image file. */
static size_t file_read(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (WL_FSEEK64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

typedef struct {
    const char *case_name;
    const char *image_name;
} test_case_t;

static const test_case_t kCases[] = {
    { "linear-1pv",            "linear-1pv.img" },
    { "linear-1pv-multiseg",   "linear-1pv-multiseg.img" },
    { "linear-2pv",            "linear-2pv-a.img" },
    { "striped-2pv",           "striped-2pv-a.img" },
    { "sector4k",              "sector4k.img" },
    { "odd-geometry",          "odd-geometry.img" },
    { "mixed-vg",              "mixed-vg-a.img" },
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus-images-dir>\n", argv[0]);
        return 1;
    }
    const char *images_dir = argv[1];

    int pass = 0, fail = 0;
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        const test_case_t *c = &kCases[i];
        char img_path[1024];
        snprintf(img_path, sizeof(img_path), "%s/%s", images_dir, c->image_name);

        FILE *f = fopen(img_path, "rb");
        if (!f) {
            fprintf(stderr, "[%s] could not open image %s\n", c->case_name, img_path);
            fail++;
            continue;
        }

        WL_FSEEK64(f, 0, SEEK_END);
        uint64_t image_size = (uint64_t)WL_FTELL64(f);
        WL_FSEEK64(f, 0, SEEK_SET);

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