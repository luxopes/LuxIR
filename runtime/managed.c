#include "managed.h"
#include "hash.h"
#include "internal/heap_hash.h"
#include "memory.h"
#include "os.h"
#include <stddef.h>
/* Freelist capacities are powers of two, starting at 64 bytes. Slabs are
 * retained for reuse until destroy. Headers never move; collection allocates
 * no mark stack. Metadata and roots are immutable/owner-local during GC. */
typedef struct segment {
    struct segment *next;
    uint64_t size, used, reserved, key0, key1;
} segment;
static const luxrt_descriptor wide_int = {LUXRT_WIDE_INT_TYPE, 0, 0, 0, 0, 0};
static const luxrt_descriptor floating = {LUXRT_FLOAT_TYPE, 0, 0, 0, 0, 0};
static uint64_t size_class(uint64_t capacity) {
    uint64_t n = 0, size = 64;
    while (size < capacity && n < 31) {
        size *= 2;
        n++;
    }
    return n;
}
int32_t luxrt_heap_init(luxrt_heap *h, uint64_t maximum, uint64_t page) {
    if (page < 64 || (page & (page - 1)) || maximum < page || maximum > UINT64_C(1099511627776))
        return -22;
    luxrt_memset(h, 0, sizeof *h);
    h->max_mapped_bytes = maximum;
    h->page_size = page;
    h->threshold = 16384;
    return 0;
}
int32_t luxrt_roots_push(luxrt_heap *h, luxrt_roots *r, luxrt_value *slots, uint64_t count) {
    if ((count && !slots) || count > UINT64_MAX / 8)
        return -22;
    /* Detect duplicate frames, which otherwise create an unbounded GC cycle. */
    luxrt_roots *old = h->roots;
    while (old) {
        if (old == r)
            return -22;
        old = old->previous;
    }
    r->previous = h->roots;
    r->slots = slots;
    r->count = count;
    h->roots = r;
    return 0;
}
int32_t luxrt_roots_pop(luxrt_heap *h, luxrt_roots *r) {
    if (h->roots != r)
        return -22;
    h->roots = r->previous;
    r->previous = 0;
    r->slots = 0;
    r->count = 0;
    return 0;
}
static int32_t mark_value(luxrt_heap *h, luxrt_value v) {
    if (v == 0 || (v & 7) == 1 || v == LUXRT_NONE || v == LUXRT_FALSE || v == LUXRT_TRUE)
        return 0;
    if (v & 7)
        return -22;
    luxrt_object *o = (luxrt_object *)(uintptr_t)v;
    if (o->owner != h || !o->descriptor)
        return -22;
    if (!o->mark) {
        o->mark = (uintptr_t)h->work | 1;
        h->work = o;
    }
    return 0;
}
static int32_t trace(luxrt_heap *h) {
    luxrt_roots *r = h->roots;
    while (r) {
        for (uint64_t i = 0; i < r->count; i++)
            if (mark_value(h, r->slots[i]))
                return -22;
        r = r->previous;
    }
    while (h->work) {
        luxrt_object *o = h->work;
        h->work = (void *)(o->mark & ~(uintptr_t)1);
        o->mark = 1;
        const luxrt_descriptor *d = o->descriptor;
        unsigned char *p = (unsigned char *)(o + 1);
        if (d->reference_count > o->payload_bytes / 8 ||
            (d->reference_count && !d->reference_offsets))
            return -22;
        for (uint64_t i = 0; i < d->reference_count; i++) {
            uint64_t off = d->reference_offsets[i];
            if ((off & 7) || off > o->payload_bytes || o->payload_bytes - off < 8)
                return -22;
            if (mark_value(h, *(luxrt_value *)(void *)(p + off)))
                return -22;
        }
        if (d->stride) {
            if (d->stride < 8 || (d->stride & 7) || (d->count_offset & 7) ||
                (d->repeat_offset & 7) || d->count_offset > o->payload_bytes ||
                o->payload_bytes - d->count_offset < 8 || d->repeat_offset > o->payload_bytes)
                return -22;
            uint64_t count = *(uint64_t *)(void *)(p + d->count_offset);
            uint64_t avail = o->payload_bytes - d->repeat_offset;
            if (count && (avail < 8 || count - 1 > (avail - 8) / d->stride))
                return -22;
            for (uint64_t i = 0; i < count; i++)
                if (mark_value(h, *(luxrt_value *)(void *)(p + d->repeat_offset + i * d->stride)))
                    return -22;
        }
    }
    return 0;
}
int32_t luxrt_heap_collect(luxrt_heap *h) {
    int32_t status = trace(h);
    if (status) {
        luxrt_object *o = h->objects;
        while (o) {
            o->mark = 0;
            o = o->next;
        }
        h->work = 0;
        return status;
    }
    luxrt_object **link = (luxrt_object **)(void *)&h->objects;
    uint64_t live = 0;
    while (*link) {
        luxrt_object *o = *link;
        if (o->mark) {
            o->mark = 0;
            live += o->capacity;
            link = (luxrt_object **)(void *)&o->next;
        } else {
            *link = o->next;
            uint64_t bin = size_class(o->capacity);
            o->descriptor = 0;
            o->next = h->free_bins[bin];
            h->free_bins[bin] = o;
        }
    }
    h->live_bytes = live;
    h->allocated_bytes = live;
    h->threshold = live > UINT64_MAX / 3 ? UINT64_MAX : live * 3;
    if (h->threshold < 16384)
        h->threshold = 16384;
    return 0;
}
static luxrt_object *reserve(luxrt_heap *h, uint64_t capacity, uint64_t bin) {
    luxrt_object *o = h->free_bins[bin];
    if (o) {
        h->free_bins[bin] = o->next;
        return o;
    }
    segment *s = h->segments;
    if (!s || capacity > s->size - s->used) {
        uint64_t bytes = capacity + sizeof(segment);
        if (bytes < 65536)
            bytes = 65536;
        if (bytes > UINT64_MAX - (h->page_size - 1))
            return 0;
        bytes = (bytes + h->page_size - 1) & ~(h->page_size - 1);
        if (bytes > h->max_mapped_bytes - h->mapped_bytes)
            return 0;
        int64_t mapped = luxrt_os_map(bytes);
        if (mapped < 0)
            return 0;
        s = (segment *)(uintptr_t)mapped;
        s->next = h->segments;
        s->size = bytes;
        s->used = sizeof(segment);
        s->reserved = s->next ? s->next->reserved : 0;
        s->key0 = s->next ? s->next->key0 : 0;
        s->key1 = s->next ? s->next->key1 : 0;
        h->segments = s;
        h->mapped_bytes += bytes;
    }
    o = (luxrt_object *)(void *)((unsigned char *)s + s->used);
    s->used += capacity;
    return o;
}
int32_t luxrt_heap_seed_hash(luxrt_heap *h, uint64_t key0, uint64_t key1) {
    if (h->objects)
        return -16;
    if (!h->segments && !reserve(h, 0, 0))
        return -12;
    segment *s = h->segments;
    while (s) {
        s->key0 = key0;
        s->key1 = key1;
        s->reserved = 1;
        s = s->next;
    }
    return 0;
}
static uint64_t hash_mix(uint64_t hash) {
    hash ^= hash >> 30;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94d049bb133111eb);
    return hash ^ (hash >> 31);
}
uint64_t luxrt_heap_hash_bytes(luxrt_heap *h, const void *data, uint64_t size) {
    segment *s = h->segments;
    if (s && s->reserved)
        return luxrt_siphash24(data, size, s->key0, s->key1);
    const unsigned char *p = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint64_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash_mix(hash);
}
uint64_t luxrt_heap_hash_word(luxrt_heap *h, uint64_t word) {
    segment *s = h->segments;
    return s && s->reserved ? luxrt_siphash24(&word, 8, s->key0, s->key1) : hash_mix(word);
}
int32_t luxrt_object_new(luxrt_heap *h, const luxrt_descriptor *d, uint64_t bytes,
                         luxrt_value *out) {
    if (!d || !d->type_id || bytes > UINT64_C(137438953424))
        return -22;
    uint64_t cap = 64, bin = 0;
    while (cap < bytes + sizeof(luxrt_object) && bin < 31) {
        cap *= 2;
        bin++;
    }
    if (cap < bytes + sizeof(luxrt_object) || cap > h->max_mapped_bytes)
        return -12;
    if (cap > h->threshold || h->allocated_bytes > h->threshold - cap) {
        int32_t status = luxrt_heap_collect(h);
        if (status)
            return status;
    }
    luxrt_object *o = reserve(h, cap, bin);
    if (!o) {
        int32_t status = luxrt_heap_collect(h);
        if (status)
            return status;
        o = reserve(h, cap, bin);
        if (!o)
            return -12;
    }
    o->descriptor = d;
    o->capacity = cap;
    o->next = h->objects;
    o->mark = 0;
    o->owner = h;
    o->payload_bytes = bytes;
    luxrt_memset(o + 1, 0, bytes);
    h->objects = o;
    h->allocated_bytes += cap;
    *out = (uintptr_t)o;
    return 0;
}
void *luxrt_object_payload(luxrt_value v) { return (luxrt_object *)(uintptr_t)v + 1; }
uint64_t luxrt_value_type(luxrt_value v) {
    if ((v & 7) == 1)
        return LUXRT_INT_TYPE;
    if (v == LUXRT_NONE)
        return LUXRT_NONE_TYPE;
    if (v == LUXRT_FALSE || v == LUXRT_TRUE)
        return LUXRT_BOOL_TYPE;
    if (v && !(v & 7)) {
        uint64_t t = ((luxrt_object *)(uintptr_t)v)->descriptor->type_id;
        return t == LUXRT_WIDE_INT_TYPE ? LUXRT_INT_TYPE : t;
    }
    return 0;
}
int32_t luxrt_value_int(luxrt_heap *h, int64_t i, luxrt_value *out) {
    if (i >= -INT64_C(1152921504606846976) && i <= INT64_C(1152921504606846975)) {
        *out = ((uint64_t)i << 3) | 1;
        return 0;
    }
    luxrt_value v;
    int32_t status = luxrt_object_new(h, &wide_int, 8, &v);
    if (status)
        return status;
    *(int64_t *)luxrt_object_payload(v) = i;
    *out = v;
    return 0;
}
int32_t luxrt_value_get_int(luxrt_value v, int64_t *out) {
    if ((v & 7) == 1) {
        *out = (int64_t)v >> 3;
        return 0;
    }
    if (!v || (v & 7) || ((luxrt_object *)(uintptr_t)v)->descriptor->type_id != LUXRT_WIDE_INT_TYPE)
        return -22;
    *out = *(int64_t *)luxrt_object_payload(v);
    return 0;
}
int32_t luxrt_value_float_bits(luxrt_heap *h, uint64_t bits, luxrt_value *out) {
    luxrt_value v;
    int32_t status = luxrt_object_new(h, &floating, 8, &v);
    if (status)
        return status;
    *(uint64_t *)luxrt_object_payload(v) = bits;
    *out = v;
    return 0;
}
int32_t luxrt_value_get_float_bits(luxrt_value v, uint64_t *out) {
    if (!v || (v & 7) || ((luxrt_object *)(uintptr_t)v)->descriptor->type_id != LUXRT_FLOAT_TYPE)
        return -22;
    *out = *(uint64_t *)luxrt_object_payload(v);
    return 0;
}
int32_t luxrt_heap_destroy(luxrt_heap *h) {
    if (h->roots)
        return -16;
    /* Preserve the segment chain on a syscall error; successful mappings
     * already removed cannot be used again even if a later unmap fails. */
    h->objects = 0;
    h->work = 0;
    luxrt_memset(h->free_bins, 0, sizeof h->free_bins);
    h->allocated_bytes = 0;
    h->live_bytes = 0;
    while (h->segments) {
        segment *s = h->segments;
        segment *next = s->next;
        uint64_t size = s->size;
        int64_t status = luxrt_os_unmap(s, size);
        if (status < 0)
            return (int32_t)status;
        h->segments = next;
        h->mapped_bytes -= size;
    }
    return 0;
}
