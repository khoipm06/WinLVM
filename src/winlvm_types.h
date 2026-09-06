/*
 * winlvm_types.h — shared freestanding types for the winlvm C layer.
 * Single definition of winlvm_byte_source so lvmfmt and luksfmt compose
 * (LUKS view feeding the LVM parser) without duplicate-typedef conflicts.
 */
#ifndef WINLVM_TYPES_H
#define WINLVM_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* A read-only, randomly-addressable byte source. The driver backs this
 * with IRP_MJ_READ down to the physical disk; user-mode tests back it
 * with fopen/fread. read() returns bytes read, or 0 on short read/fail. */
typedef struct winlvm_byte_source {
    void *impl;
    uint64_t size;
    size_t (*read)(void *impl, uint64_t offset, void *buf, size_t len);
} winlvm_byte_source;

#endif
