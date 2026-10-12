#ifndef LUXRT_ARENA_H
#define LUXRT_ARENA_H
#include <stdint.h>
/* Raw, caller-owned 24-byte arena. Single owner; no GC and no OS allocation.
 * Descriptor, backing memory and output slots must be valid and disjoint.
 * init: -22 invalid null range, -75 address wrap. alloc: -22 zero size or
 * invalid alignment, -12 exhaustion. Errors leave descriptor/output intact.
 * Successful allocations are uninitialized. Rewind invalidates newer blocks;
 * marks are arena.used snapshots from the same uninterrupted arena lifetime.
 */
typedef struct { void *base; uint64_t capacity, used; } luxrt_arena;
int32_t luxrt_arena_init(luxrt_arena *arena, void *base, uint64_t capacity);
int32_t luxrt_arena_alloc(luxrt_arena *arena, uint64_t size, uint64_t alignment, void **out);
int32_t luxrt_arena_rewind(luxrt_arena *arena, uint64_t mark);
#endif
