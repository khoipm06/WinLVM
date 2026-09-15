/*
 * arena.c — bump allocator over caller-provided scratch buffer.
 */
#include "lvmcore/arena.h"

void lvmcore_arena_init(lvmcore_arena *arena, void *buffer, size_t capacity) {
    if (!arena) return;
    arena->buffer = (uint8_t *)buffer;
    arena->capacity = buffer ? capacity : 0;
    arena->offset = 0;
}

void *lvmcore_arena_alloc(lvmcore_arena *arena, size_t size, size_t align) {
    if (!arena || !arena->buffer) return NULL;

    if (align == 0) {
        align = sizeof(void *);
    }
    /* align must be a power of two */
    if ((align & (align - 1)) != 0) {
        return NULL;
    }

    if (arena->offset > arena->capacity) {
        return NULL;
    }

    uintptr_t current = (uintptr_t)(arena->buffer + arena->offset);
    uintptr_t mask = (uintptr_t)(align - 1);
    uintptr_t aligned = (current + mask) & ~mask;

    /* Check for integer overflow in alignment calculation */
    if (aligned < current) {
        return NULL;
    }

    size_t padding = (size_t)(aligned - current);

    /* Check for overflow in offset + padding */
    if (padding > arena->capacity - arena->offset) {
        return NULL;
    }
    size_t aligned_offset = arena->offset + padding;

    /* Check for overflow in aligned_offset + size */
    if (size > arena->capacity - aligned_offset) {
        return NULL;
    }

    arena->offset = aligned_offset + size;
    return (void *)aligned;
}

void *lvmcore_arena_alloc_zero(lvmcore_arena *arena, size_t size, size_t align) {
    void *ptr = lvmcore_arena_alloc(arena, size, align);
    if (ptr && size > 0) {
        uint8_t *b = (uint8_t *)ptr;
        for (size_t i = 0; i < size; i++) {
            b[i] = 0;
        }
    }
    return ptr;
}

void lvmcore_arena_reset(lvmcore_arena *arena) {
    if (arena) {
        arena->offset = 0;
    }
}

size_t lvmcore_arena_available(const lvmcore_arena *arena) {
    if (!arena || arena->offset >= arena->capacity) return 0;
    return arena->capacity - arena->offset;
}

size_t lvmcore_arena_used(const lvmcore_arena *arena) {
    if (!arena) return 0;
    return arena->offset;
}
