#include "../../runtime/map.h"
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c))                                                                                  \
            return 1;                                                                              \
    } while (0)
int main(void) {
    luxrt_heap h;
    luxrt_roots frame;
    luxrt_value r[12] = {0};
    CHECK(luxrt_heap_init(&h, 16777216, 4096) == 0);
    CHECK(luxrt_roots_push(&h, &frame, r, 12) == 0);
    CHECK(luxrt_string_new(&h, "one,,three,", 11, r) == 0);
    CHECK(luxrt_string_new(&h, ",", 1, r + 1) == 0);
    CHECK(luxrt_string_split(&h, r[0], r[1], r + 2) == 0);
    uint64_t n = 0;
    CHECK(luxrt_vector_length(r[2], &n) == 0 && n == 4);
    CHECK(luxrt_values_get(r[2], 2, r + 3) == 0);
    CHECK(luxrt_string_new(&h, "three", 5, r + 4) == 0);
    int32_t equal = 0;
    CHECK(luxrt_string_equal(r[3], r[4], &equal) == 0 && equal);
    CHECK(luxrt_array_new(&h, LUXRT_INT_ARRAY_TYPE, 0, r + 5) == 0);
    for (uint64_t i = 0; i < 10000; i++)
        CHECK(luxrt_array_append(&h, r[5], i) == 0);
    uint64_t sum = 0;
    for (uint64_t i = 0; i < 10000; i++) {
        uint64_t item;
        CHECK(luxrt_array_get(r[5], i, &item) == 0);
        sum += item;
    }
    CHECK(sum == 49995000);
    CHECK(luxrt_array_new(&h, LUXRT_FLOAT_ARRAY_TYPE, 0, r + 7) == 0);
    CHECK(luxrt_array_append(&h, r[7], UINT64_C(0xfff800000000abcd)) == 0);
    CHECK(luxrt_map_new(&h, 1, r + 6) == 0);
    for (uint64_t i = 0; i < 10000; i++) {
        luxrt_value key = ((i % 1024) << 3) | 1, value = (i << 3) | 1;
        CHECK(luxrt_map_set(&h, r[6], key, value) == 0);
    }
    CHECK(luxrt_heap_collect(&h) == 0);
    for (uint64_t i = 0; i < 1024; i++) {
        luxrt_value value;
        CHECK(luxrt_map_get(r[6], (i << 3) | 1, &value) == 0);
        CHECK(value == ((((9999 - i) / 1024 * 1024 + i) << 3) | 1));
    }
    for (uint64_t i = 0; i < 1024; i += 2) {
        luxrt_value old;
        CHECK(luxrt_map_delete(r[6], (i << 3) | 1, &old) == 0);
    }
    CHECK(luxrt_map_set(&h, r[6], 1, LUXRT_TRUE) == 0);
    uint64_t cursor = 0;
    luxrt_value key, value;
    for (uint64_t i = 1; i < 1024; i += 2) {
        CHECK(luxrt_map_next(r[6], &cursor, &key, &value) == 0);
        CHECK(key == ((i << 3) | 1));
    }
    CHECK(luxrt_map_next(r[6], &cursor, &key, &value) == 0 && key == 1);
    CHECK(luxrt_map_next(r[6], &cursor, &key, &value) == -2);
    CHECK(luxrt_value_float_bits(&h, UINT64_C(0x3ff0000000000000), r + 8) == 0);
    CHECK(luxrt_map_get(r[6], r[8], &value) == 0 && value == ((9217UL << 3) | 1));
    CHECK(luxrt_set_new(&h, 1, r + 9) == 0);
    CHECK(luxrt_set_add(&h, r[9], LUXRT_TRUE) == 0);
    CHECK(luxrt_set_add(&h, r[9], 9) == 0);
    CHECK(luxrt_map_length(r[9], &n) == 0 && n == 1);
    CHECK(luxrt_heap_collect(&h) == 0);
    uint64_t bits = 0;
    CHECK(luxrt_array_get(r[7], 0, &bits) == 0 && bits == UINT64_C(0xfff800000000abcd));
    CHECK(luxrt_roots_pop(&h, &frame) == 0);
    CHECK(luxrt_heap_destroy(&h) == 0);
    return 0;
}
