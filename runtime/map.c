#include "map.h"
#include "memory.h"
#include <stddef.h>
#define MAP_PAYLOAD(v) ((void *)((luxrt_object *)(uintptr_t)(v) + 1))
typedef struct {
    uint64_t length, used, capacity, index_slots, flags;
    luxrt_value entries, hashes, index;
} ordered_map;
typedef struct {
    uint64_t length, capacity;
    uint64_t data[];
} map_buffer;
static const uint64_t map_refs[] = {40, 48, 56};
static const luxrt_descriptor map_desc = {LUXRT_MAP_TYPE, 3, map_refs, 0, 0, 0};
static const luxrt_descriptor set_desc = {LUXRT_SET_TYPE, 3, map_refs, 0, 0, 0};
static const luxrt_descriptor entry_desc = {34, 0, 0, 16, 0, 8};
static const luxrt_descriptor index_desc = {35, 0, 0, 0, 0, 0};
static ordered_map *get_map(luxrt_value v) {
    if (!v || (v & 7))
        return 0;
    luxrt_object *object = (luxrt_object *)(uintptr_t)v;
    uint64_t type = object->descriptor ? object->descriptor->type_id : 0;
    return type == LUXRT_MAP_TYPE || type == LUXRT_SET_TYPE ? MAP_PAYLOAD(v) : 0;
}
static int32_t own(luxrt_heap *h, luxrt_value v) {
    if ((v & 7) == 1 || v == LUXRT_NONE || v == LUXRT_FALSE || v == LUXRT_TRUE)
        return 1;
    if (!v || (v & 7))
        return 0;
    luxrt_object *o = (luxrt_object *)(uintptr_t)v;
    return o->owner == h && o->descriptor;
}
static uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
static int32_t integer_key(luxrt_value v, uint64_t flags, int64_t *out) {
    if (luxrt_value_get_int(v, out) == 0)
        return 1;
    if (!(flags & LUXRT_MAP_NUMERIC_KEYS))
        return 0;
    if (v == LUXRT_FALSE || v == LUXRT_TRUE) {
        *out = v == LUXRT_TRUE;
        return 1;
    }
    uint64_t bits;
    if (luxrt_value_get_float_bits(v, &bits))
        return 0;
    union {
        uint64_t bits;
        double value;
    } f;
    f.bits = bits;
    if (f.value >= -9223372036854775808.0 && f.value < 9223372036854775808.0) {
        int64_t i = (int64_t)f.value;
        if ((double)i == f.value) {
            *out = i;
            return 1;
        }
    }
    return 0;
}
typedef struct {
    uint64_t bytes, characters, hash;
    unsigned char data[];
} cached_string;
/* Runtime-private access to the documented managed header, not a frontend
 * requirement. Avoid out-parameter helpers and repeated type tests in probes. */
static int32_t key_hash(luxrt_value v, uint64_t flags, uint64_t *out) {
    uint64_t type = luxrt_value_type(v);
    if (type == LUXRT_STRING_TYPE) {
        *out = ((cached_string *)MAP_PAYLOAD(v))->hash ^ UINT64_C(0x8f21754953);
        return 0;
    }
    int64_t i;
    if (integer_key(v, flags, &i)) {
        *out = mix((uint64_t)i ^ UINT64_C(0x0123456789abcdef));
        return 0;
    }
    if (type == LUXRT_NONE_TYPE || type == LUXRT_BOOL_TYPE) {
        *out = mix(v);
        return 0;
    }
    if (type == LUXRT_FLOAT_TYPE) {
        uint64_t bits;
        luxrt_value_get_float_bits(v, &bits);
        if ((bits << 1) == 0)
            bits = 0;
        *out = mix(bits ^ UINT64_C(0xf01a712d98));
        return 0;
    }
    return -22;
}
static int32_t key_equal(luxrt_value a, luxrt_value b, uint64_t flags) {
    if (a == b)
        return 1;
    uint64_t type = luxrt_value_type(a), other = luxrt_value_type(b);
    if (type == LUXRT_STRING_TYPE && other == type) {
        cached_string *x = MAP_PAYLOAD(a), *y = MAP_PAYLOAD(b);
        return x->bytes == y->bytes && x->hash == y->hash &&
               luxrt_memcmp(x->data, y->data, x->bytes) == 0;
    }
    int64_t x, y;
    int32_t ix = integer_key(a, flags, &x), iy = integer_key(b, flags, &y);
    if (ix || iy)
        return ix && iy && x == y;
    if (type != other)
        return 0;
    if (type == LUXRT_FLOAT_TYPE) {
        union {
            uint64_t bits;
            double value;
        } p, q;
        luxrt_value_get_float_bits(a, &p.bits);
        luxrt_value_get_float_bits(b, &q.bits);
        return p.value == q.value;
    }
    return 0;
}
/* Return slot position;
 *entry is UINT64_MAX for a missing key. */
static uint64_t probe(ordered_map *m, luxrt_value key, uint64_t hash, uint64_t *entry) {
    map_buffer *entries = MAP_PAYLOAD(m->entries), *hashes = MAP_PAYLOAD(m->hashes),
               *index = MAP_PAYLOAD(m->index);
    uint64_t slot = hash & (m->index_slots - 1), first = UINT64_MAX;
    for (uint64_t n = 0; n < m->index_slots; n++) {
        uint64_t position = index->data[slot];
        if (!position) {
            *entry = UINT64_MAX;
            return first == UINT64_MAX ? slot : first;
        }
        if (position == UINT64_MAX) {
            if (first == UINT64_MAX)
                first = slot;
        } else {
            uint64_t e = position - 1;
            if (hashes->data[e] == hash && key_equal(entries->data[2 * e], key, m->flags)) {
                *entry = e;
                return slot;
            }
        }
        slot = (slot + 1) & (m->index_slots - 1);
    }
    *entry = UINT64_MAX;
    return first;
}
static int32_t rebuild(luxrt_heap *h, luxrt_value v, uint64_t capacity) {
    ordered_map *m = get_map(v);
    if (capacity > (UINT64_MAX - 16) / 16 || capacity < m->length || capacity > UINT64_MAX / 2)
        return -34;
    uint64_t slots = capacity * 2;
    luxrt_value roots[4] = {v, 0, 0, 0};
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, roots, 4);
    if (status)
        return status;
    status = luxrt_object_new(h, &entry_desc, 16 + capacity * 16, roots + 1);
    if (!status)
        status = luxrt_object_new(h, &index_desc, 16 + capacity * 8, roots + 2);
    if (!status)
        status = luxrt_object_new(h, &index_desc, 16 + slots * 8, roots + 3);
    if (!status) {
        map_buffer *entries = MAP_PAYLOAD(roots[1]), *hashes = MAP_PAYLOAD(roots[2]),
                   *index = MAP_PAYLOAD(roots[3]);
        uint64_t count = 0;
        entries->capacity = capacity * 2;
        hashes->capacity = capacity;
        index->capacity = slots;
        if (m->entries) {
            map_buffer *old_entries = MAP_PAYLOAD(m->entries), *old_hashes = MAP_PAYLOAD(m->hashes);
            for (uint64_t i = 0; i < m->used; i++) {
                if (!old_entries->data[2 * i])
                    continue;
                entries->data[2 * count] = old_entries->data[2 * i];
                entries->data[2 * count + 1] = old_entries->data[2 * i + 1];
                uint64_t hash = old_hashes->data[i];
                hashes->data[count] = hash;
                uint64_t slot = hash & (slots - 1);
                while (index->data[slot])
                    slot = (slot + 1) & (slots - 1);
                index->data[slot] = count + 1;
                count++;
            }
        }
        entries->length = count * 2;
        m->used = count;
        m->capacity = capacity;
        m->index_slots = slots;
        m->entries = roots[1];
        m->hashes = roots[2];
        m->index = roots[3];
    }
    luxrt_roots_pop(h, &frame);
    return status;
}
int32_t luxrt_map_new(luxrt_heap *h, uint64_t flags, luxrt_value *out) {
    if (flags & ~LUXRT_MAP_NUMERIC_KEYS)
        return -22;
    luxrt_value v;
    int32_t status = luxrt_object_new(h, &map_desc, sizeof(ordered_map), &v);
    if (status)
        return status;
    ((ordered_map *)MAP_PAYLOAD(v))->flags = flags;
    status = rebuild(h, v, 8);
    if (status)
        return status;
    *out = v;
    return 0;
}
int32_t luxrt_map_set(luxrt_heap *h, luxrt_value v, luxrt_value key, luxrt_value value) {
    ordered_map *m = get_map(v);
    if (!m || !own(h, v) || !own(h, key) || !own(h, value))
        return -22;
    uint64_t hash;
    int32_t status = key_hash(key, m->flags, &hash);
    if (status)
        return status;
    uint64_t e, slot = probe(m, key, hash, &e);
    if (e != UINT64_MAX) {
        ((map_buffer *)MAP_PAYLOAD(m->entries))->data[2 * e + 1] = value;
        return 0;
    }
    luxrt_value roots[3] = {v, key, value};
    luxrt_roots frame;
    status = luxrt_roots_push(h, &frame, roots, 3);
    if (status)
        return status;
    if (m->used == m->capacity) {
        uint64_t cap = m->length < m->capacity / 2 ? m->capacity : m->capacity * 2;
        status = cap < m->capacity ? -34 : rebuild(h, v, cap);
        if (!status)
            slot = probe(m, key, hash, &e);
    }
    if (!status) {
        if (slot == UINT64_MAX)
            status = -12;
        else {
            map_buffer *entries = MAP_PAYLOAD(m->entries), *hashes = MAP_PAYLOAD(m->hashes),
                       *index = MAP_PAYLOAD(m->index);
            entries->data[2 * m->used] = key;
            entries->data[2 * m->used + 1] = value;
            hashes->data[m->used] = hash;
            index->data[slot] = m->used + 1;
            m->used++;
            m->length++;
            entries->length = m->used * 2;
        }
    }
    luxrt_roots_pop(h, &frame);
    return status;
}
int32_t luxrt_map_get(luxrt_value v, luxrt_value key, luxrt_value *out) {
    ordered_map *m = get_map(v);
    if (!m)
        return -22;
    uint64_t hash;
    int32_t status = key_hash(key, m->flags, &hash);
    if (status)
        return status;
    uint64_t e;
    probe(m, key, hash, &e);
    if (e == UINT64_MAX)
        return -2;
    *out = ((map_buffer *)MAP_PAYLOAD(m->entries))->data[2 * e + 1];
    return 0;
}
int32_t luxrt_map_delete(luxrt_value v, luxrt_value key, luxrt_value *old) {
    ordered_map *m = get_map(v);
    if (!m)
        return -22;
    uint64_t hash;
    int32_t status = key_hash(key, m->flags, &hash);
    if (status)
        return status;
    uint64_t e, slot = probe(m, key, hash, &e);
    if (e == UINT64_MAX)
        return -2;
    map_buffer *entries = MAP_PAYLOAD(m->entries);
    *old = entries->data[2 * e + 1];
    entries->data[2 * e] = 0;
    entries->data[2 * e + 1] = 0;
    ((map_buffer *)MAP_PAYLOAD(m->index))->data[slot] = UINT64_MAX;
    m->length--;
    return 0;
}
int32_t luxrt_map_length(luxrt_value v, uint64_t *out) {
    ordered_map *m = get_map(v);
    if (!m)
        return -22;
    *out = m->length;
    return 0;
}
int32_t luxrt_map_next(luxrt_value v, uint64_t *cursor, luxrt_value *key, luxrt_value *value) {
    ordered_map *m = get_map(v);
    if (!m)
        return -22;
    if (*cursor > m->used)
        return -34;
    map_buffer *entries = MAP_PAYLOAD(m->entries);
    uint64_t i = *cursor;
    while (i < m->used && !entries->data[2 * i])
        i++;
    if (i == m->used)
        return -2;
    *key = entries->data[2 * i];
    *value = entries->data[2 * i + 1];
    *cursor = i + 1;
    return 0;
}
int32_t luxrt_map_clear(luxrt_value v) {
    ordered_map *m = get_map(v);
    if (!m)
        return -22;
    map_buffer *entries = MAP_PAYLOAD(m->entries), *index = MAP_PAYLOAD(m->index);
    luxrt_memset(entries->data, 0, m->used * 16);
    luxrt_memset(index->data, 0, m->index_slots * 8);
    entries->length = 0;
    m->length = 0;
    m->used = 0;
    return 0;
}
int32_t luxrt_set_new(luxrt_heap *h, uint64_t flags, luxrt_value *out) {
    luxrt_value v;
    int32_t status = luxrt_map_new(h, flags, &v);
    if (status)
        return status;
    ((luxrt_object *)(uintptr_t)v)->descriptor = &set_desc;
    *out = v;
    return 0;
}
int32_t luxrt_set_add(luxrt_heap *h, luxrt_value v, luxrt_value key) {
    if (luxrt_value_type(v) != LUXRT_SET_TYPE)
        return -22;
    return luxrt_map_set(h, v, key, LUXRT_NONE);
}
int32_t luxrt_set_contains(luxrt_value v, luxrt_value key, int32_t *out) {
    if (luxrt_value_type(v) != LUXRT_SET_TYPE)
        return -22;
    luxrt_value value;
    int32_t status = luxrt_map_get(v, key, &value);
    if (status && status != -2)
        return status;
    *out = status == 0;
    return 0;
}
int32_t luxrt_set_remove(luxrt_value v, luxrt_value key) {
    if (luxrt_value_type(v) != LUXRT_SET_TYPE)
        return -22;
    luxrt_value value;
    return luxrt_map_delete(v, key, &value);
}
