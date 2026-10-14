/*
 * arena.h — freestanding bump allocator over caller-supplied memory.
 *
 * Used throughout lvmcore to guarantee zero dynamic allocations
 * (zero malloc, free, ExAllocatePool). Kernel-safe, non-recursive,
 * strictly bounds-checked against integer overflow.
 */
#ifndef LVMCORE_ARENA_H
#define LVMCORE_ARENA_H

#include "lvmcore/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lvmcore_arena {
    uint8_t *buffer;
    size_t   capacity;
    size_t   offset;
} lvmcore_arena;

/*
 * Initialize arena over caller-supplied buffer of length capacity.
 * The buffer is owned by the caller.
 */
void lvmcore_arena_init(lvmcore_arena *arena, void *buffer, size_t capacity);

/*
 * Allocate `size` bytes aligned to `align` from the arena.
 * `align` must be a power of two (or 0 for default pointer alignment).
 * Returns NULL if the arena is exhausted or on integer overflow.
 */
void *lvmcore_arena_alloc(lvmcore_arena *arena, size_t size, size_t align);

/*
 * Allocate `size` bytes aligned to `align` and zero-initialize the memory.
 * Returns NULL if the arena is exhausted or on integer overflow.
 */
void *lvmcore_arena_alloc_zero(lvmcore_arena *arena, size_t size, size_t align);

/*
 * Reset arena offset back to 0. Does not modify or free the underlying buffer.
 */
void lvmcore_arena_reset(lvmcore_arena *arena);

/*
 * Return number of remaining bytes that can be allocated (unaligned).
 */
size_t lvmcore_arena_available(const lvmcore_arena *arena);

/*
 * Return number of bytes currently allocated (including alignment padding).
 */
size_t lvmcore_arena_used(const lvmcore_arena *arena);

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_ARENA_H */
