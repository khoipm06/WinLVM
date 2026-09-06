/*
 * test_luks_read.c — user-mode harness for luks_read.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WL_FSEEK64 _fseeki64
#define WL_FTELL64 _ftelli64
#else
#define WL_FSEEK64 fseeko
#define WL_FTELL64 ftello
#endif

#include "luks_read.h"

static size_t file_read(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (WL_FSEEK64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <luks-image>\n", argv[0]);
        return 1;
    }
    const char *img_path = argv[1];

    FILE *f = fopen(img_path, "rb");
    if (!f) {
        perror(img_path);
        return 1;
    }
    WL_FSEEK64(f, 0, SEEK_END);
    uint64_t image_size = (uint64_t)WL_FTELL64(f);
    WL_FSEEK64(f, 0, SEEK_SET);

    winlvm_byte_source src = {
        .impl = f,
        .size = image_size,
        .read = file_read,
    };

    winlvm_luks_metadata md;
    winlvm_luks_err err = {0};
    if (!winlvm_luks_read_metadata(&src, &md, &err)) {
        fprintf(stderr, "LUKS2 read failed: %s\n", err.message);
        fclose(f);
        return 1;
    }
    fclose(f);

    printf("LUKS2 metadata parsed OK.\n");
    printf("Segments: %d\n", md.segment_count);
    for (int i = 0; i < md.segment_count; i++) {
        winlvm_luks_segment *s = &md.segments[i];
        printf("  [%d] type=%s offset=%llu size=%llu iv_tweak=%llu sector_size=%llu encryption=%s\n",
               i, s->type, (unsigned long long)s->offset, (unsigned long long)s->size,
               (unsigned long long)s->iv_tweak, (unsigned long long)s->sector_size,
               s->encryption);
    }
    printf("Digests: %d\n", md.digest_count);
    for (int i = 0; i < md.digest_count; i++) {
        winlvm_luks_digest *d = &md.digests[i];
        printf("  [%d] type=%s hash=%s iterations=%llu\n",
               i, d->type, d->kdf.hash, (unsigned long long)d->kdf.iterations);
    }
    printf("Keyslots: %d\n", md.keyslot_count);
    for (int i = 0; i < md.keyslot_count; i++) {
        winlvm_luks_keyslot *k = &md.keyslots[i];
        printf("  [%d] type=%s priority=%llu kdf.type=%s kdf.hash=%s area_offset=%llu area_size=%llu\n",
               i, k->type, (unsigned long long)k->priority,
               k->kdf.type, k->kdf.hash,
               (unsigned long long)k->area_offset, (unsigned long long)k->area_size);
    }
    return 0;
}
