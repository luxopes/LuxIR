#include "../../runtime/numeric.h"
#include "../../runtime/tuple.h"
#include "../../runtime/collections.h"
#include "../../runtime/integer.h"
#include "../../runtime/float64.h"
#include "../../runtime/memory.h"
#define CHECK(c) do { checks++; if (!(c)) return __LINE__; } while (0)
static uint64_t random_step(uint64_t *seed) {
    uint64_t x = *seed;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *seed = x; return x;
}
int32_t luxrt_extended_cases(uint64_t *outchecks) {
    uint64_t checks = 0, seed = UINT64_C(0x79f91835e169234b);
    luxrt_heap h, foreign, fail;
    luxrt_roots frame;
    luxrt_value r[16] = {0}, original[128], result;
    CHECK(luxrt_heap_init(&h, 16777216, 4096) == 0);
    CHECK(luxrt_heap_init(&foreign, 16777216, 4096) == 0);
    CHECK(luxrt_heap_init(&fail, 4096, 4096) == 0);
    CHECK(luxrt_roots_push(&h, &frame, r, 16) == 0);
    CHECK(luxrt_string_new(&h, "retained", 8, r + 4) == 0);
    for (uint64_t i=0; i<10000; i++) {
        int64_t integer = (int64_t)random_step(&seed), got;
        CHECK(luxrt_value_int(&h, integer, r) == 0);
        CHECK(luxrt_int_to_string(&h, r[0], r+1) == 0);
        CHECK(luxrt_string_to_int(&h, r[1], r+2) == 0);
        CHECK(luxrt_value_get_int(r[2], &got) == 0 && got == integer);
        uint64_t n,chars,hash;
        const void *data;
        unsigned char expected[32];
        int64_t written = luxrt_format_i64(integer, expected, 32);
        CHECK(luxrt_string_info(r[1], &n, &chars, &data, &hash) == 0);
        CHECK(n == (uint64_t)written && chars == n && !luxrt_memcmp(data, expected, n));
        uint64_t bits = random_step(&seed), parsed;
        if ((bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000)) bits &= UINT64_C(0xffefffffffffffff);
        CHECK(luxrt_value_float_bits(&h, bits, r) == 0);
        CHECK(luxrt_float_to_string(&h, r[0], r+1) == 0);
        CHECK(luxrt_string_to_float(&h, r[1], r+2) == 0);
        CHECK(luxrt_value_get_float_bits(r[2], &parsed) == 0 && parsed == bits);
        written = luxrt_format_float64(bits, expected, 32);
        CHECK(luxrt_string_info(r[1], &n, &chars, &data, &hash) == 0);
        CHECK(n == (uint64_t)written && chars == n && !luxrt_memcmp(data, expected, n));
        if (!(i & 63)) CHECK(luxrt_heap_collect(&h) == 0);
    }
    const char *bad[] = {"", "1x", " 1", "1 ", "--1", "1_2"};
    uint64_t lengths[] = {0, 2, 2, 2, 3, 3};
    for (uint64_t i=0; i<6; i++) {
        CHECK(luxrt_string_new(&h, bad[i], lengths[i], r) == 0);
        result = LUXRT_TRUE;
        CHECK(luxrt_string_to_int(&h, r[0], &result) == -22 && result == LUXRT_TRUE);
        CHECK(luxrt_string_to_float(&h, r[0], &result) == -22 && result == LUXRT_TRUE);
    }
    CHECK(luxrt_string_new(&h, "1\0", 2, r) == 0);
    CHECK(luxrt_string_to_float(&h, r[0], &result) == -22 && result == LUXRT_TRUE);
    CHECK(luxrt_string_to_int(&h, r[0], &result) == -22 && result == LUXRT_TRUE);
    CHECK(luxrt_string_new(&h, "9223372036854775808", 19, r) == 0);
    CHECK(luxrt_string_to_int(&h, r[0], &result) == -34 && result == LUXRT_TRUE);
    CHECK(luxrt_string_new(&h, "-0.0", 4, r) == 0);
    CHECK(luxrt_string_to_float(&h, r[0], r+1) == 0);
    uint64_t bits;
    CHECK(luxrt_value_get_float_bits(r[1], &bits) == 0 && bits == UINT64_C(0x8000000000000000));
    CHECK(luxrt_int_to_string(&h, LUXRT_TRUE, &result) == -22 && result == LUXRT_TRUE);
    CHECK(luxrt_float_to_string(&h, 1, &result) == -22 && result == LUXRT_TRUE);
    CHECK(luxrt_string_to_int(&h, LUXRT_NONE, &result) == -22 && result == LUXRT_TRUE);
    CHECK(luxrt_int_to_string(&fail, 1, &result) == -12 && result == LUXRT_TRUE);
    CHECK(luxrt_tuple_new(&fail, 0, 0, &result) == -12 && result == LUXRT_TRUE);
    CHECK(luxrt_string_new(&foreign, "other", 5, &result) == 0);
    original[0] = result; result = LUXRT_TRUE;
    CHECK(luxrt_tuple_new(&h, original, 1, &result) == -22 && result == LUXRT_TRUE);
    CHECK(luxrt_tuple_new(&h, 0, 1, &result) == -22 && result == LUXRT_TRUE);
    CHECK(luxrt_tuple_new(&h, 0, UINT64_MAX, &result) == -34 && result == LUXRT_TRUE);
    original[0] = 0;
    CHECK(luxrt_tuple_new(&h, original, 1, &result) == -22 && result == LUXRT_TRUE);
    for (uint64_t iteration=0; iteration<5000; iteration++) {
        uint64_t n = random_step(&seed) % 128;
        for (uint64_t i=0; i<n; i++) original[i] = i % 5 == 0 ? r[4] : ((random_step(&seed) >> 3) << 3) | 1;
        h.threshold = 0;
        CHECK(luxrt_tuple_new(&h, original, n, r+5) == 0);
        uint64_t length;
        CHECK(luxrt_tuple_length(r[5], &length) == 0 && length == n);
        for (uint64_t i=0; i<n; i++) CHECK(luxrt_tuple_get(r[5], i, &result) == 0 && result == original[i]);
        result = LUXRT_TRUE;
        CHECK(luxrt_tuple_get(r[5], n, &result) == -34 && result == LUXRT_TRUE);
        uint64_t begin = random_step(&seed) % (n+1), end = begin + random_step(&seed) % (n-begin+1);
        CHECK(luxrt_tuple_slice(&h, r[5], begin, end, r+6) == 0);
        CHECK(luxrt_tuple_concat(&h, r[6], r[5], r+7) == 0);
        CHECK(luxrt_tuple_length(r[7], &length) == 0 && length == end-begin+n);
        CHECK(luxrt_heap_collect(&h) == 0);
        for (uint64_t i=0; i<end-begin+n; i++) CHECK(luxrt_tuple_get(r[7], i, &result) == 0 && result == original[i<end-begin ? begin+i : i-(end-begin)]);
        CHECK(luxrt_values_new(&h, n, r+8) == 0);
        for (uint64_t i=0; i<n; i++) CHECK(luxrt_values_append(&h, r[8], original[i]) == 0);
        CHECK(luxrt_tuple_from_values(&h, r[8], r+9) == 0);
        CHECK(luxrt_values_clear(r[8]) == 0);
        CHECK(luxrt_heap_collect(&h) == 0);
        for (uint64_t i=0; i<n; i++) CHECK(luxrt_tuple_get(r[9], i, &result) == 0 && result == original[i]);
        result = LUXRT_TRUE;
        CHECK(luxrt_tuple_slice(&h, r[5], n+1, n+1, &result) == -34 && result == LUXRT_TRUE);
        CHECK(luxrt_tuple_from_values(&h, r[5], &result) == -22 && result == LUXRT_TRUE);
    }
    for (uint64_t i=0; i<16; i++) r[i] = 0;
    CHECK(luxrt_heap_collect(&h) == 0 && h.live_bytes == 0);
    CHECK(luxrt_roots_pop(&h, &frame) == 0);
    CHECK(luxrt_heap_destroy(&h) == 0);
    CHECK(luxrt_heap_destroy(&foreign) == 0);
    CHECK(luxrt_heap_destroy(&fail) == 0);
    *outchecks = checks;
    return 0;
}
