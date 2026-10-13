#ifndef LUXRT_MANAGED_H
#define LUXRT_MANAGED_H
#include <stdint.h>
/* Experimental managed core v1, 64-bit little-endian targets.
 * One heap has one owner. Calls may collect: root every live managed Value
 * in a linked root frame before allocating; no conservative stack scanning.
 * Values and metadata must be valid, never forged; zero is a vacant root.
 */
typedef uint64_t luxrt_value;
#define LUXRT_NONE UINT64_C(2)
#define LUXRT_FALSE UINT64_C(3)
#define LUXRT_TRUE UINT64_C(11)
#define LUXRT_INT_TYPE UINT64_C(1)
#define LUXRT_NONE_TYPE UINT64_C(2)
#define LUXRT_BOOL_TYPE UINT64_C(3)
#define LUXRT_WIDE_INT_TYPE UINT64_C(16)
#define LUXRT_FLOAT_TYPE UINT64_C(17)
#define LUXRT_STRING_TYPE UINT64_C(18)
#define LUXRT_VALUES_TYPE UINT64_C(19)
#define LUXRT_MAP_TYPE UINT64_C(20)
#define LUXRT_SET_TYPE UINT64_C(21)
/* Offsets are relative to the payload following the 48-byte header. Repeated
 * Value slots use length at count_offset; stride==0 disables the tail map. */
typedef struct {
    uint64_t type_id, reference_count;
    const uint64_t *reference_offsets;
    uint64_t repeat_offset, count_offset, stride;
} luxrt_descriptor;
typedef struct luxrt_roots {
    struct luxrt_roots *previous;
    luxrt_value *slots;
    uint64_t count;
} luxrt_roots;
typedef struct luxrt_heap {
    void *objects, *segments;
    luxrt_roots *roots;
    uint64_t mapped_bytes, allocated_bytes, live_bytes, threshold;
    uint64_t max_mapped_bytes, page_size;
    void *work;
    void *free_bins[32];
} luxrt_heap;
typedef struct {
    const luxrt_descriptor *descriptor;
    uint64_t capacity;
    void *next;
    uintptr_t mark;
    luxrt_heap *owner;
    uint64_t payload_bytes;
} luxrt_object;
int32_t luxrt_heap_init(luxrt_heap *,uint64_t max_mapped_bytes,uint64_t page_size);
/* Opt-in keyed SipHash-2-4 for String/Map/Set hashes. Call before constructing
 * any objects, or after a collection leaves no live objects (-16 otherwise).
 * No automatic entropy acquisition; use a secret unpredictable 128-bit key.
 * May allocate the first slab (-12 on failure); existing seed unchanged on failure.
 * Destroy removes seed. Heap context size and Value/object layouts are unchanged. */
int32_t luxrt_heap_seed_hash(luxrt_heap *,uint64_t key0,uint64_t key1);
/* Initialize a fresh heap with 128 bits from OS getrandom. Handles partial
 * reads/EINTR, fails closed on entropy errors/zero reads, output unchanged on
 * failure. May allocate the first slab. Fresh/destroyed contexts are required. */
int32_t luxrt_heap_init_secure(luxrt_heap *,uint64_t max_mapped_bytes,uint64_t page_size);
int32_t luxrt_heap_destroy(luxrt_heap *); /* no outstanding root frames */
int32_t luxrt_roots_push(luxrt_heap *,luxrt_roots *,luxrt_value *,uint64_t);
int32_t luxrt_roots_pop(luxrt_heap *,luxrt_roots *);
int32_t luxrt_heap_collect(luxrt_heap *);
int32_t luxrt_object_new(luxrt_heap *,const luxrt_descriptor *,uint64_t payload_bytes,luxrt_value *out);
void *luxrt_object_payload(luxrt_value); /* only a live managed object */
uint64_t luxrt_value_type(luxrt_value);
int32_t luxrt_value_int(luxrt_heap *,int64_t,luxrt_value *out);
int32_t luxrt_value_get_int(luxrt_value,int64_t *out);
int32_t luxrt_value_float_bits(luxrt_heap *,uint64_t,luxrt_value *out);
int32_t luxrt_value_get_float_bits(luxrt_value,uint64_t *out);
#endif
