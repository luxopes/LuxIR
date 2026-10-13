#include "tuple.h"
#include "collections.h"
#include "memory.h"
static const luxrt_descriptor tuple_desc = {LUXRT_TUPLE_TYPE, 0, 0, 8, 0, 8};
typedef struct {
    uint64_t length;
    luxrt_value items[];
} tuple;
static tuple *get_tuple(luxrt_value v) {
    return luxrt_value_type(v) == LUXRT_TUPLE_TYPE ? luxrt_object_payload(v) : 0;
}
static int32_t owned(luxrt_heap *h, luxrt_value v) {
    if ((v & 7) == 1 || v == LUXRT_NONE || v == LUXRT_FALSE || v == LUXRT_TRUE)
        return 1;
    if (!v || (v & 7))
        return 0;
    luxrt_object *o = (luxrt_object *)(uintptr_t)v;
    return o->owner == h && o->descriptor;
}
int32_t luxrt_tuple_new(luxrt_heap *h, const luxrt_value *items, uint64_t count, luxrt_value *out) {
    if (count > (UINT64_MAX - 8) / 8)
        return -34;
    if (count && !items)
        return -22;
    for (uint64_t i = 0; i < count; i++)
        if (!owned(h, items[i]))
            return -22;
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, (luxrt_value *)items, count);
    if (status)
        return status;
    luxrt_value v;
    status = luxrt_object_new(h, &tuple_desc, 8 + count * 8, &v);
    if (!status) {
        tuple *t = luxrt_object_payload(v);
        t->length = count;
        luxrt_memcpy(t->items, items, count * 8);
        *out = v;
    }
    luxrt_roots_pop(h, &frame);
    return status;
}
int32_t luxrt_tuple_length(luxrt_value v, uint64_t *out) {
    tuple *t = get_tuple(v);
    if (!t)
        return -22;
    *out = t->length;
    return 0;
}
int32_t luxrt_tuple_get(luxrt_value v, uint64_t index, luxrt_value *out) {
    tuple *t = get_tuple(v);
    if (!t)
        return -22;
    if (index >= t->length)
        return -34;
    *out = t->items[index];
    return 0;
}
int32_t luxrt_tuple_slice(luxrt_heap *h, luxrt_value v, uint64_t begin, uint64_t end,
                          luxrt_value *out) {
    tuple *t = get_tuple(v);
    if (!t || !owned(h, v))
        return -22;
    if (begin > end || end > t->length)
        return -34;
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, &v, 1);
    if (status)
        return status;
    status = luxrt_tuple_new(h, t->items + begin, end - begin, out);
    luxrt_roots_pop(h, &frame);
    return status;
}
int32_t luxrt_tuple_concat(luxrt_heap *h, luxrt_value a, luxrt_value b, luxrt_value *out) {
    tuple *ta = get_tuple(a), *tb = get_tuple(b);
    if (!ta || !tb || !owned(h, a) || !owned(h, b))
        return -22;
    if (ta->length > (UINT64_MAX - 8) / 8 - tb->length)
        return -34;
    luxrt_value roots[2] = {a, b}, v;
    luxrt_roots frame;
    int32_t status = luxrt_roots_push(h, &frame, roots, 2);
    if (status)
        return status;
    uint64_t count = ta->length + tb->length;
    status = luxrt_object_new(h, &tuple_desc, 8 + count * 8, &v);
    if (!status) {
        tuple *t = luxrt_object_payload(v);
        t->length = count;
        luxrt_memcpy(t->items, ta->items, ta->length * 8);
        luxrt_memcpy(t->items + ta->length, tb->items, tb->length * 8);
        *out = v;
    }
    luxrt_roots_pop(h, &frame);
    return status;
}
int32_t luxrt_tuple_from_values(luxrt_heap *h, luxrt_value v, luxrt_value *out) {
    if (luxrt_value_type(v) != LUXRT_VALUES_TYPE || !owned(h, v))
        return -22;
    uint64_t count;
    int32_t status = luxrt_vector_length(v, &count);
    if (status)
        return status;
    if (count > (UINT64_MAX - 8) / 8)
        return -34;
    luxrt_roots frame;
    status = luxrt_roots_push(h, &frame, &v, 1);
    if (status)
        return status;
    luxrt_value result;
    status = luxrt_object_new(h, &tuple_desc, 8 + count * 8, &result);
    if (!status) {
        tuple *t = luxrt_object_payload(result);
        t->length = count;
        for (uint64_t i = 0; i < count; i++)
            luxrt_values_get(v, i, t->items + i);
        *out = result;
    }
    luxrt_roots_pop(h, &frame);
    return status;
}
