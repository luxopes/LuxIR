#include "../../runtime/map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "collections failure %d: %s\n", __LINE__, #c);                         \
            exit(1);                                                                               \
        }                                                                                          \
        checks++;                                                                                  \
    } while (0)
static unsigned checks;
static uint64_t seed = UINT64_C(0x71ead30ff2);
static uint64_t rnd(void) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}
extern uint64_t test_mapped_bytes;
static void check_string(luxrt_value v, const void *text, uint64_t n, uint64_t chars) {
    uint64_t size = 0, length = 0, hash = 0;
    const void *data = 0;
    CHECK(luxrt_string_info(v, &size, &length, &data, &hash) == 0);
    CHECK(size == n && length == chars);
    CHECK(!memcmp(data, text, (size_t)n));
    CHECK(((const unsigned char *)data)[n] == 0);
}
int main(void) {
    luxrt_heap h;
    luxrt_roots frame;
    luxrt_value r[16] = {0};
    CHECK(luxrt_heap_init(&h, 64 * 1024 * 1024, 4096) == 0);
    CHECK(luxrt_roots_push(&h, &frame, r, 16) == 0);
    const unsigned char text[] = {0xc5, 0xbe, 0, 0xe2, 0x82, 0xac, 0xf0, 0x90, 0x90, 0x80};
    CHECK(luxrt_string_new(&h, text, sizeof text, r) == 0);
    CHECK(luxrt_string_new(&h, text, sizeof text, r + 1) == 0);
    check_string(r[0], text, sizeof text, 4);
    int32_t equal = 42;
    CHECK(luxrt_string_equal(r[0], r[1], &equal) == 0 && equal == 1);
    CHECK(luxrt_string_compare(r[0], r[1], &equal) == 0 && equal == 0);
    CHECK(luxrt_string_concat(&h, r[0], r[1], r + 2) == 0);
    unsigned char doubled[sizeof text * 2];
    memcpy(doubled, text, sizeof text);
    memcpy(doubled + sizeof text, text, sizeof text);
    check_string(r[2], doubled, sizeof doubled, 8);
    CHECK(luxrt_string_slice(&h, r[0], 1, 3, r + 3) == 0);
    check_string(r[3], text + 2, 4, 2);
    CHECK(luxrt_string_slice(&h, r[0], 4, 4, r + 3) == 0);
    check_string(r[3], "", 0, 0);
    luxrt_value out = LUXRT_NONE;
    CHECK(luxrt_string_slice(&h, r[0], 3, 2, &out) == -34 && out == LUXRT_NONE);
    const unsigned char invalid[] = {0xc0, 0x80};
    CHECK(luxrt_string_new(&h, invalid, 2, &out) == -84 && out == LUXRT_NONE);
    CHECK(luxrt_string_new(&h, NULL, UINT64_MAX, &out) == -34 && out == LUXRT_NONE);
    CHECK(luxrt_string_new(&h, "a,,b,", 5, r + 2) == 0);
    CHECK(luxrt_string_new(&h, ",", 1, r + 3) == 0);
    CHECK(luxrt_string_split(&h, r[2], r[3], r + 4) == 0);
    uint64_t n = 0;
    CHECK(luxrt_vector_length(r[4], &n) == 0 && n == 4);
    const char *parts[] = {"a", "", "b", ""};
    for (unsigned i = 0; i < 4; i++) {
        CHECK(luxrt_values_get(r[4], i, &out) == 0);
        check_string(out, parts[i], strlen(parts[i]), strlen(parts[i]));
    }
    CHECK(luxrt_values_clear(r[4]) == 0);
    luxrt_value model[1024];
    unsigned length = 0;
    for (unsigned i = 0; i < 100000; i++) {
        unsigned op = (unsigned)(rnd() % 6);
        luxrt_value v = ((rnd() % 10000) << 3) | 1;
        unsigned index = length ? (unsigned)(rnd() % length) : 0;
        if (op < 2 && length < 1024) {
            CHECK(luxrt_values_append(&h, r[4], v) == 0);
            model[length++] = v;
        } else if (op == 2) {
            out = LUXRT_NONE;
            int status = luxrt_values_pop(r[4], &out);
            CHECK(status == (length ? 0 : -34));
            CHECK(out == (length ? model[--length] : LUXRT_NONE));
        } else if (op == 3) {
            out = LUXRT_NONE;
            int status = luxrt_values_get(r[4], index, &out);
            CHECK(status == (length ? 0 : -34));
            CHECK(out == (length ? model[index] : LUXRT_NONE));
        } else if (op == 4) {
            int status = luxrt_values_set(&h, r[4], index, v);
            CHECK(status == (length ? 0 : -34));
            if (length)
                model[index] = v;
        } else if (op == 5 && i % 1021 == 0) {
            CHECK(luxrt_values_clear(r[4]) == 0);
            length = 0;
        }
        CHECK(luxrt_vector_length(r[4], &n) == 0 && n == length);
        if (i % 997 == 0) {
            CHECK(luxrt_heap_collect(&h) == 0);
            for (unsigned j = 0; j < length; j++) {
                CHECK(luxrt_values_get(r[4], j, &out) == 0 && out == model[j]);
            }
        }
    }
    /* GC sees only Value slots: all bits in numeric array elements remain raw. */
    for (uint64_t kind = LUXRT_INT_ARRAY_TYPE; kind <= LUXRT_BYTE_ARRAY_TYPE; kind++) {
        CHECK(luxrt_array_new(&h, kind, 0, r + 7) == 0);
        uint64_t expected[256];
        length = 0;
        for (unsigned i = 0; i < 20000; i++) {
            unsigned op = (unsigned)(rnd() % 4);
            uint64_t value = rnd();
            if (kind == LUXRT_BYTE_ARRAY_TYPE)
                value &= 255;
            unsigned index = length ? (unsigned)(rnd() % length) : 0;
            uint64_t bits = 42;
            if (op == 0 && length < 256) {
                CHECK(luxrt_array_append(&h, r[7], value) == 0);
                expected[length++] = value;
            } else if (op == 1) {
                int status = luxrt_array_pop(r[7], &bits);
                CHECK(status == (length ? 0 : -34));
                CHECK(bits == (length ? expected[--length] : 42));
            } else if (op == 2) {
                int status = luxrt_array_get(r[7], index, &bits);
                CHECK(status == (length ? 0 : -34));
                CHECK(bits == (length ? expected[index] : 42));
            } else if (op == 3) {
                int status = luxrt_array_set(r[7], index, value);
                CHECK(status == (length ? 0 : -34));
                if (length)
                    expected[index] = value;
            }
            CHECK(luxrt_vector_length(r[7], &n) == 0 && n == length);
            if (i % 127 == 0)
                CHECK(luxrt_heap_collect(&h) == 0);
        }
    }
    CHECK(luxrt_array_append(&h, r[7], 256) == -34);
    CHECK(luxrt_values_new(&h, UINT64_MAX, &out) == -34);
    /* Insertion order is independently maintained as a compact key sequence. */
    CHECK(luxrt_map_new(&h, 0, r + 5) == 0);
    enum { K = 512 };
    unsigned char present[K] = {0};
    luxrt_value values[K];
    unsigned order[K], count = 0;
    for (unsigned i = 0; i < 100000; i++) {
        unsigned key = (unsigned)(rnd() % K), op = (unsigned)(rnd() % 4);
        luxrt_value k = ((uint64_t)key << 3) | 1, v = ((rnd() % 100000) << 3) | 1;
        out = LUXRT_NONE;
        if (op == 0) {
            CHECK(luxrt_map_set(&h, r[5], k, v) == 0);
            if (!present[key]) {
                present[key] = 1;
                order[count++] = key;
            }
            values[key] = v;
        } else if (op == 1) {
            int status = luxrt_map_delete(r[5], k, &out);
            CHECK(status == (present[key] ? 0 : -2));
            CHECK(out == (present[key] ? values[key] : LUXRT_NONE));
            if (present[key]) {
                present[key] = 0;
                unsigned j = 0;
                while (order[j] != key)
                    j++;
                memmove(order + j, order + j + 1, (count - j - 1) * sizeof(unsigned));
                count--;
            }
        } else {
            int status = luxrt_map_get(r[5], k, &out);
            CHECK(status == (present[key] ? 0 : -2));
            CHECK(out == (present[key] ? values[key] : LUXRT_NONE));
        }
        CHECK(luxrt_map_length(r[5], &n) == 0 && n == count);
        if (i % 503 == 0) {
            CHECK(luxrt_heap_collect(&h) == 0);
            uint64_t cursor = 0;
            for (unsigned j = 0; j < count; j++) {
                luxrt_value actual_key, actual_value;
                CHECK(luxrt_map_next(r[5], &cursor, &actual_key, &actual_value) == 0);
                CHECK(actual_key == (((uint64_t)order[j] << 3) | 1) &&
                      actual_value == values[order[j]]);
            }
            CHECK(luxrt_map_next(r[5], &cursor, &out, &out) == -2);
        }
    }
    CHECK(luxrt_map_clear(r[5]) == 0);
    memset(present, 0, sizeof present);
    count = 0;
    /* Newly allocated equal string keys must replace, not append. */
    for (unsigned i = 0; i < 10000; i++) {
        unsigned key = (unsigned)(rnd() % K);
        char text_key[32];
        int bytes = snprintf(text_key, sizeof text_key, "key_%u", key);
        CHECK(bytes > 0);
        CHECK(luxrt_string_new(&h, text_key, (uint64_t)bytes, r + 6) == 0);
        luxrt_value val = ((uint64_t)i << 3) | 1;
        CHECK(luxrt_map_set(&h, r[5], r[6], val) == 0);
        if (!present[key]) {
            present[key] = 1;
            count++;
        }
        values[key] = val;
        out = LUXRT_NONE;
        CHECK(luxrt_map_get(r[5], r[6], &out) == 0 && out == val);
        CHECK(luxrt_map_length(r[5], &n) == 0 && n == count);
        if (i % 97 == 0)
            CHECK(luxrt_heap_collect(&h) == 0);
    }
    /* Numeric policy covers exact large integers, -0, NaN identity and Bool. */
    CHECK(luxrt_map_new(&h, LUXRT_MAP_NUMERIC_KEYS, r + 8) == 0);
    CHECK(luxrt_value_int(&h, 1, r + 9) == 0);
    CHECK(luxrt_value_float_bits(&h, UINT64_C(0x3ff0000000000000), r + 10) == 0);
    CHECK(luxrt_map_set(&h, r[8], LUXRT_TRUE, r[0]) == 0);
    CHECK(luxrt_map_set(&h, r[8], r[9], r[1]) == 0);
    CHECK(luxrt_map_set(&h, r[8], r[10], r[2]) == 0);
    CHECK(luxrt_map_length(r[8], &n) == 0 && n == 1);
    CHECK(luxrt_map_get(r[8], LUXRT_TRUE, &out) == 0 && out == r[2]);
    CHECK(luxrt_value_int(&h, 9007199254740993L, r + 9) == 0);
    CHECK(luxrt_value_float_bits(&h, UINT64_C(0x4340000000000000), r + 10) == 0);
    CHECK(luxrt_map_set(&h, r[8], r[9], LUXRT_TRUE) == 0);
    CHECK(luxrt_map_set(&h, r[8], r[10], LUXRT_FALSE) == 0);
    CHECK(luxrt_map_length(r[8], &n) == 0 && n == 3);
    CHECK(luxrt_value_float_bits(&h, UINT64_C(0x7ff8000000000001), r + 9) == 0);
    CHECK(luxrt_value_float_bits(&h, UINT64_C(0x7ff8000000000001), r + 10) == 0);
    CHECK(luxrt_map_set(&h, r[8], r[9], LUXRT_TRUE) == 0);
    CHECK(luxrt_map_set(&h, r[8], r[10], LUXRT_FALSE) == 0);
    CHECK(luxrt_map_get(r[8], r[9], &out) == 0 && out == LUXRT_TRUE);
    CHECK(luxrt_map_length(r[8], &n) == 0 && n == 5);
    CHECK(luxrt_map_new(&h, 0, r + 11) == 0);
    CHECK(luxrt_map_set(&h, r[11], LUXRT_TRUE, LUXRT_TRUE) == 0);
    CHECK(luxrt_value_int(&h, 1, r + 9) == 0);
    CHECK(luxrt_map_set(&h, r[11], r[9], LUXRT_FALSE) == 0);
    CHECK(luxrt_map_length(r[11], &n) == 0 && n == 2);
    CHECK(luxrt_set_new(&h, 1, r + 12) == 0);
    CHECK(luxrt_set_add(&h, r[12], LUXRT_TRUE) == 0);
    CHECK(luxrt_set_add(&h, r[12], r[9]) == 0);
    CHECK(luxrt_map_length(r[12], &n) == 0 && n == 1);
    int32_t found = 42;
    CHECK(luxrt_set_contains(r[12], r[9], &found) == 0 && found == 1);
    CHECK(luxrt_set_remove(r[12], LUXRT_TRUE) == 0);
    CHECK(luxrt_set_contains(r[12], r[9], &found) == 0 && found == 0);
    CHECK(luxrt_map_set(&h, r[11], r[4], LUXRT_NONE) == -22);
    CHECK(luxrt_heap_collect(&h) == 0);
    memset(r, 0, sizeof r);
    CHECK(luxrt_heap_collect(&h) == 0 && !h.live_bytes);
    CHECK(luxrt_roots_pop(&h, &frame) == 0);
    CHECK(luxrt_heap_destroy(&h) == 0 && !test_mapped_bytes);
    printf("%u collections checks passed\n", checks);
    return 0;
}
