#include "../../runtime/managed.h"
int32_t gc_force(luxrt_heap *h) {
    if (!h->roots) return -99;
    return luxrt_heap_collect(h);
}
int32_t gc_fail(luxrt_heap *h) {
    int32_t status = gc_force(h);
    return status ? status : -22;
}
luxrt_value gc_make_box(luxrt_heap *h, int64_t n) {
    luxrt_value v = LUXRT_NONE;
    h->threshold = 0;
    if (luxrt_value_int(h, n, &v)) return LUXRT_NONE;
    return v;
}
uint64_t gc_xor_boxes(luxrt_value a, luxrt_value b) {
    int64_t x = 0, y = 0;
    if (luxrt_value_get_int(a, &x) || luxrt_value_get_int(b, &y)) return 0;
    return (uint64_t)x ^ (uint64_t)y;
}
luxrt_value gc_parameter(luxrt_heap *, luxrt_value);
uint64_t gc_locals(luxrt_heap *, int64_t);
luxrt_value gc_phi(luxrt_heap *, luxrt_value, int32_t);
uint64_t gc_borrow(luxrt_heap *, luxrt_value);
int32_t gc_error(luxrt_heap *, luxrt_value);
luxrt_value gc_leaf(luxrt_heap *, luxrt_value);
#define CHECK(c) do { checks++; if (!(c)) return __LINE__; } while (0)
int32_t luxrt_gcroot_cases(uint64_t *out) {
    uint64_t checks = 0;
    luxrt_heap h;
    CHECK(luxrt_heap_init(&h, 1048576, 4096) == 0);
    for (uint64_t i=0; i<5000; i++) {
        int64_t expected = (int64_t)(UINT64_C(0x7fffffffffffffff)-i), number;
        luxrt_value v;
        CHECK(luxrt_value_int(&h, expected, &v) == 0);
        CHECK(h.roots == 0);
        luxrt_value result = gc_parameter(&h, v);
        CHECK(result == v && h.roots == 0);
        CHECK(luxrt_value_get_int(result, &number) == 0 && number == expected);
        result = gc_phi(&h, v, i&1);
        CHECK(result == v && h.roots == 0);
        CHECK(luxrt_value_get_int(result, &number) == 0 && number == expected);
        CHECK(gc_borrow(&h, v) == (uint64_t)expected && h.roots == 0);
        CHECK(gc_error(&h, v) == -22 && h.roots == 0);
        CHECK(gc_leaf(&h, v) == v && h.roots == 0);
        CHECK(luxrt_heap_collect(&h) == 0 && h.live_bytes == 0);
        CHECK(gc_locals(&h, expected) == ((uint64_t)expected ^ ((uint64_t)expected+1)) && h.roots == 0);
        CHECK(luxrt_heap_collect(&h) == 0 && h.live_bytes == 0);
    }
    CHECK(luxrt_heap_destroy(&h) == 0);
    *out = checks;
    return 0;
}
