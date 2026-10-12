#include "arena.h"
int32_t luxrt_arena_init(luxrt_arena *arena, void *base, uint64_t capacity)
{
    uintptr_t address = (uintptr_t)base;
    if (!base && capacity) return -22;
    if (capacity > UINT64_MAX - address) return -75;
    arena->base = base;
    arena->capacity = capacity;
    arena->used = 0;
    return 0;
}
int32_t luxrt_arena_alloc(luxrt_arena *arena, uint64_t size, uint64_t alignment, void **out)
{
    uint64_t address, padding, remaining, used;
    if (!size || !alignment || (alignment & (alignment - 1))) return -22;
    /* Invariant established by init; only these APIs may mutate the arena. */
    used = arena->used;
    remaining = arena->capacity - used;
    address = (uintptr_t)arena->base + used;
    padding = (UINT64_C(0) - address) & (alignment - 1);
    if (padding > remaining || size > remaining - padding) return -12;
    *out = (void *)(uintptr_t)(address + padding);
    arena->used = used + padding + size;
    return 0;
}
int32_t luxrt_arena_rewind(luxrt_arena *arena, uint64_t mark)
{
    if (mark > arena->used) return -22;
    arena->used = mark;
    return 0;
}
