#include "collections.h"
#include "memory.h"
#include <stddef.h>
static const uint64_t vector_refs[] = {16};
static const luxrt_descriptor string_desc = {LUXRT_STRING_TYPE, 0, 0, 0, 0, 0};
static const luxrt_descriptor values_desc = {LUXRT_VALUES_TYPE, 1, vector_refs, 0, 0, 0};
static const luxrt_descriptor int_array_desc = {LUXRT_INT_ARRAY_TYPE, 1, vector_refs, 0, 0, 0};
static const luxrt_descriptor float_array_desc = {LUXRT_FLOAT_ARRAY_TYPE, 1, vector_refs, 0, 0, 0};
static const luxrt_descriptor byte_array_desc = {LUXRT_BYTE_ARRAY_TYPE, 1, vector_refs, 0, 0, 0};
static const luxrt_descriptor refs_buffer_desc = {32, 0, 0, 16, 0, 8};
static const luxrt_descriptor raw_buffer_desc = {33, 0, 0, 0, 0, 0};
typedef struct {
    uint64_t length, capacity;
    luxrt_value backing;
} vector;
typedef struct {
    uint64_t bytes, characters, hash;
    unsigned char data[];
} string;
typedef struct {
    uint64_t length, capacity;
    luxrt_value slots[];
} buffer;
static luxrt_object *object_of(luxrt_value v, uint64_t kind) {
    if (!v || (v & 7))
        return 0;
    luxrt_object *o = (luxrt_object *)(uintptr_t)v;
    return o->descriptor && o->descriptor->type_id == kind ? o : 0;
}
static int32_t owned(luxrt_heap *h, luxrt_value v) {
    if ((v & 7) == 1 || v == LUXRT_NONE || v == LUXRT_FALSE || v == LUXRT_TRUE)
        return 1;
    if (!v || (v & 7))
        return 0;
    luxrt_object *o = (luxrt_object *)(uintptr_t)v;
    return o->owner == h && o->descriptor;
}
static string *get_string(luxrt_value v) {
    return object_of(v, LUXRT_STRING_TYPE) ? luxrt_object_payload(v) : 0;
}
static uint64_t hash_string(const unsigned char *p, uint64_t n) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint64_t i = 0; i < n; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    /* Avalanche once at construction; map probes consume the cached hash. */
    hash ^= hash >> 30;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94d049bb133111eb);
    return hash ^ (hash >> 31);
}
static uint64_t character_count(const unsigned char *p, uint64_t n) {
    uint64_t count = 0;
    for (uint64_t i = 0; i < n; i++)
        if ((p[i] & 192) != 128)
            count++;
    return count;
}
int32_t luxrt_string_new(luxrt_heap *h, const void *data, uint64_t n, luxrt_value *out) {
    if ((n && !data) || n > UINT64_MAX - sizeof(string) - 1)
        return -34;
    if (!luxrt_utf8_valid(data, n))
        return -84;
    luxrt_value v;
    int32_t status = luxrt_object_new(h, &string_desc, sizeof(string) + n + 1, &v);
    if (status)
        return status;
    string *s = luxrt_object_payload(v);
    s->bytes = n;
    s->characters = character_count(data, n);
    luxrt_memcpy(s->data, data, n);
    s->data[n] = 0;
    s->hash = hash_string(s->data, n);
    *out = v;
    return 0;
}
int32_t luxrt_string_info(luxrt_value v, uint64_t *bytes, uint64_t *characters, const void **data,
                          uint64_t *hash) {
    string *s = get_string(v);
    if (!s)
        return -22;
    *bytes = s->bytes;
    *characters = s->characters;
    *data = s->data;
    *hash = s->hash;
    return 0;
}
int32_t luxrt_string_equal(luxrt_value a, luxrt_value b, int32_t *out) {
    string *x = get_string(a), *y = get_string(b);
    if (!x || !y)
        return -22;
    *out = a == b || (x->bytes == y->bytes && x->hash == y->hash &&
                      luxrt_memcmp(x->data, y->data, x->bytes) == 0);
    return 0;
}
int32_t luxrt_string_compare(luxrt_value a, luxrt_value b, int32_t *out) {
    string *x = get_string(a), *y = get_string(b);
    if (!x || !y)
        return -22;
    uint64_t n = x->bytes < y->bytes ? x->bytes : y->bytes;
    int32_t c = luxrt_memcmp(x->data, y->data, n);
    *out = c ? c : x->bytes < y->bytes ? -1 : x->bytes > y->bytes ? 1 : 0;
    return 0;
}
int32_t luxrt_string_concat(luxrt_heap *h, luxrt_value a, luxrt_value b, luxrt_value *out) {
    string *x = get_string(a), *y = get_string(b);
    if (!x || !y || !owned(h, a) || !owned(h, b))
        return -22;
    if (x->bytes > UINT64_MAX - sizeof(string) - 1 - y->bytes)
        return -34;
    luxrt_value roots[2] = {a, b};
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, roots, 2);
    if (status)
        return status;
    luxrt_value v;
    uint64_t n = x->bytes + y->bytes;
    status = luxrt_object_new(h, &string_desc, sizeof(string) + n + 1, &v);
    if (!status) {
        string *s = luxrt_object_payload(v);
        s->bytes = n;
        s->characters = x->characters + y->characters;
        luxrt_memcpy(s->data, x->data, x->bytes);
        luxrt_memcpy(s->data + x->bytes, y->data, y->bytes);
        s->data[n] = 0;
        s->hash = hash_string(s->data, n);
        *out = v;
    }
    luxrt_roots_pop(h, &frame);
    return status;
}
int32_t luxrt_string_slice(luxrt_heap *h, luxrt_value v, uint64_t begin, uint64_t end,
                           luxrt_value *out) {
    string *s = get_string(v);
    if (!s || !owned(h, v))
        return -22;
    if (begin > end || end > s->characters)
        return -34;
    uint64_t i = 0, character = 0, start = 0, finish = 0;
    while (i < s->bytes) {
        if (character == begin)
            start = i;
        if (character == end) {
            finish = i;
            break;
        }
        unsigned char c = s->data[i];
        i += c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
        character++;
    }
    if (begin == s->characters)
        start = s->bytes;
    if (end == s->characters)
        finish = s->bytes;
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, &v, 1);
    if (status)
        return status;
    status = luxrt_string_new(h, s->data + start, finish - start, out);
    luxrt_roots_pop(h, &frame);
    return status;
}
static vector *get_vector(luxrt_value v) {
    uint64_t kind = luxrt_value_type(v);
    return kind == LUXRT_VALUES_TYPE || kind == LUXRT_INT_ARRAY_TYPE ||
                   kind == LUXRT_FLOAT_ARRAY_TYPE || kind == LUXRT_BYTE_ARRAY_TYPE
               ? luxrt_object_payload(v)
               : 0;
}
static int32_t grow(luxrt_heap *h, luxrt_value v, uint64_t capacity) {
    vector *a = get_vector(v);
    uint64_t kind = luxrt_value_type(v);
    uint64_t width = kind == LUXRT_BYTE_ARRAY_TYPE ? 1 : 8;
    if (!a || !owned(h, v))
        return -22;
    if (capacity > (UINT64_MAX - 16) / width)
        return -34;
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, &v, 1);
    if (status)
        return status;
    luxrt_value backing;
    status = luxrt_object_new(h, kind == LUXRT_VALUES_TYPE ? &refs_buffer_desc : &raw_buffer_desc,
                              16 + capacity * width, &backing);
    if (!status) {
        buffer *b = luxrt_object_payload(backing);
        b->length = a->length;
        b->capacity = capacity;
        if (a->length) {
            buffer *old = luxrt_object_payload(a->backing);
            luxrt_memcpy(b->slots, old->slots, a->length * width);
        }
        a->backing = backing;
        a->capacity = capacity;
    }
    luxrt_roots_pop(h, &frame);
    return status;
}
static int32_t new_vector(luxrt_heap *h, const luxrt_descriptor *d, uint64_t capacity,
                          luxrt_value *out) {
    luxrt_value v;
    int32_t status = luxrt_object_new(h, d, sizeof(vector), &v);
    if (status)
        return status;
    if (capacity) {
        status = grow(h, v, capacity);
        if (status)
            return status;
    }
    *out = v;
    return 0;
}
int32_t luxrt_values_new(luxrt_heap *h, uint64_t capacity, luxrt_value *out) {
    return new_vector(h, &values_desc, capacity, out);
}
int32_t luxrt_vector_length(luxrt_value v, uint64_t *out) {
    vector *a = get_vector(v);
    if (!a)
        return -22;
    *out = a->length;
    return 0;
}
int32_t luxrt_values_append(luxrt_heap *h, luxrt_value v, luxrt_value item) {
    vector *a = object_of(v, LUXRT_VALUES_TYPE) ? luxrt_object_payload(v) : 0;
    if (!a || !owned(h, v) || !owned(h, item))
        return -22;
    luxrt_value roots[2] = {v, item};
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, roots, 2);
    if (status)
        return status;
    if (a->length == a->capacity) {
        uint64_t n = a->capacity ? a->capacity * 2 : 8;
        status = n < a->capacity ? -34 : grow(h, v, n);
    }
    if (!status) {
        buffer *b = luxrt_object_payload(a->backing);
        b->slots[a->length] = item;
        a->length++;
        b->length = a->length;
    }
    luxrt_roots_pop(h, &frame);
    return status;
}
int32_t luxrt_values_get(luxrt_value v, uint64_t index, luxrt_value *out) {
    vector *a = object_of(v, LUXRT_VALUES_TYPE) ? luxrt_object_payload(v) : 0;
    if (!a)
        return -22;
    if (index >= a->length)
        return -34;
    *out = ((buffer *)luxrt_object_payload(a->backing))->slots[index];
    return 0;
}
int32_t luxrt_values_set(luxrt_heap *h, luxrt_value v, uint64_t index, luxrt_value item) {
    vector *a = object_of(v, LUXRT_VALUES_TYPE) ? luxrt_object_payload(v) : 0;
    if (!a || !owned(h, v) || !owned(h, item))
        return -22;
    if (index >= a->length)
        return -34;
    ((buffer *)luxrt_object_payload(a->backing))->slots[index] = item;
    return 0;
}
int32_t luxrt_values_pop(luxrt_value v, luxrt_value *out) {
    vector *a = object_of(v, LUXRT_VALUES_TYPE) ? luxrt_object_payload(v) : 0;
    if (!a)
        return -22;
    if (!a->length)
        return -34;
    buffer *b = luxrt_object_payload(a->backing);
    *out = b->slots[a->length - 1];
    b->slots[a->length - 1] = 0;
    a->length--;
    b->length = a->length;
    return 0;
}
int32_t luxrt_values_clear(luxrt_value v) {
    vector *a = object_of(v, LUXRT_VALUES_TYPE) ? luxrt_object_payload(v) : 0;
    if (!a)
        return -22;
    if (a->backing) {
        buffer *b = luxrt_object_payload(a->backing);
        luxrt_memset(b->slots, 0, a->length * 8);
        b->length = 0;
    }
    a->length = 0;
    return 0;
}
int32_t luxrt_array_new(luxrt_heap *h, uint64_t kind, uint64_t length, luxrt_value *out) {
    const luxrt_descriptor *d = kind == LUXRT_INT_ARRAY_TYPE     ? &int_array_desc
                                : kind == LUXRT_FLOAT_ARRAY_TYPE ? &float_array_desc
                                : kind == LUXRT_BYTE_ARRAY_TYPE  ? &byte_array_desc
                                                                 : 0;
    if (!d)
        return -22;
    luxrt_value v;
    int32_t status = new_vector(h, d, length, &v);
    if (status)
        return status;
    ((vector *)luxrt_object_payload(v))->length = length;
    *out = v;
    return 0;
}
static vector *get_array(luxrt_value v) {
    uint64_t k = luxrt_value_type(v);
    return k == LUXRT_INT_ARRAY_TYPE || k == LUXRT_FLOAT_ARRAY_TYPE || k == LUXRT_BYTE_ARRAY_TYPE
               ? luxrt_object_payload(v)
               : 0;
}
int32_t luxrt_array_append(luxrt_heap *h, luxrt_value v, uint64_t bits) {
    vector *a = get_array(v);
    if (!a || !owned(h, v))
        return -22;
    uint64_t kind = luxrt_value_type(v);
    if (kind == LUXRT_BYTE_ARRAY_TYPE && bits > 255)
        return -34;
    int32_t status = 0;
    if (a->length == a->capacity) {
        uint64_t n = a->capacity ? a->capacity * 2 : 8;
        status = n < a->capacity ? -34 : grow(h, v, n);
    }
    if (status)
        return status;
    buffer *b = luxrt_object_payload(a->backing);
    if (kind == LUXRT_BYTE_ARRAY_TYPE)
        ((unsigned char *)b->slots)[a->length] = (unsigned char)bits;
    else
        b->slots[a->length] = bits;
    a->length++;
    return 0;
}
int32_t luxrt_array_get(luxrt_value v, uint64_t index, uint64_t *out) {
    vector *a = get_array(v);
    if (!a)
        return -22;
    if (index >= a->length)
        return -34;
    buffer *b = luxrt_object_payload(a->backing);
    *out = luxrt_value_type(v) == LUXRT_BYTE_ARRAY_TYPE ? ((unsigned char *)b->slots)[index]
                                                        : b->slots[index];
    return 0;
}
int32_t luxrt_array_set(luxrt_value v, uint64_t index, uint64_t bits) {
    vector *a = get_array(v);
    if (!a)
        return -22;
    if (index >= a->length)
        return -34;
    uint64_t kind = luxrt_value_type(v);
    if (kind == LUXRT_BYTE_ARRAY_TYPE && bits > 255)
        return -34;
    buffer *b = luxrt_object_payload(a->backing);
    if (kind == LUXRT_BYTE_ARRAY_TYPE)
        ((unsigned char *)b->slots)[index] = (unsigned char)bits;
    else
        b->slots[index] = bits;
    return 0;
}
int32_t luxrt_array_pop(luxrt_value v, uint64_t *out) {
    vector *a = get_array(v);
    if (!a)
        return -22;
    if (!a->length)
        return -34;
    int32_t status = luxrt_array_get(v, a->length - 1, out);
    if (!status)
        a->length--;
    return status;
}
int32_t luxrt_string_split(luxrt_heap *h, luxrt_value v, luxrt_value delimiter, luxrt_value *out) {
    string *s = get_string(v), *d = get_string(delimiter);
    if (!s || !d || !d->bytes || !owned(h, v) || !owned(h, delimiter))
        return -22;
    luxrt_value roots[4] = {v, delimiter, 0, 0};
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, roots, 4);
    if (status)
        return status;
    status = luxrt_values_new(h, 0, roots + 2);
    uint64_t start = 0, pos = 0;
    while (!status && pos <= s->bytes) {
        int32_t end = pos == s->bytes;
        int32_t match = !end && d->bytes <= s->bytes - pos &&
                        luxrt_memcmp(s->data + pos, d->data, d->bytes) == 0;
        if (end || match) {
            status = luxrt_string_new(h, s->data + start, pos - start, roots + 3);
            if (!status)
                status = luxrt_values_append(h, roots[2], roots[3]);
            roots[3] = 0;
            if (end)
                break;
            pos += d->bytes;
            start = pos;
        } else
            pos++;
    }
    if (!status)
        *out = roots[2];
    luxrt_roots_pop(h, &frame);
    return status;
}
