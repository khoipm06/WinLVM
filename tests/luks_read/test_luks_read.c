/*
 * test_luks_read.c — user-mode harness for luks_read.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luks_read.h"

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

    unsigned char buf[4096];
    if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        fprintf(stderr, "failed to read 4096 bytes\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    winlvm_luks_header hdr;
    winlvm_luks_err err;
    if (!winlvm_luks_read_header(buf, sizeof(buf), &hdr, &err)) {
        fprintf(stderr, "LUKS2 parse failed: %s\n", err.message);
        return 1;
    }

    printf("LUKS2 Header:\n");
    printf("  Magic: %.*s\n", 6, hdr.magic);
    printf("  Version: %u\n", hdr.version);
    printf("  Header Size: %llu\n", (unsigned long long)hdr.hdr_size);
    printf("  Seq ID: %llu\n", (unsigned long long)hdr.seqid);
    printf("  Label: %s\n", hdr.label);
    printf("  Subsystem: %s\n", hdr.subsystem);
    printf("  UUID: %s\n", hdr.uuid);
    printf("  Checksum Alg: %s\n", hdr.checksum_alg);
    printf("  Offset: %llu\n", (unsigned long long)hdr.hdr_offset);

    return 0;
}
