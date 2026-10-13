#include "../../runtime/collections.h"
#include "../../runtime/hash.h"
#include "../../runtime/io.h"
#include "../../runtime/map.h"
#include "siphash_vectors.h"
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            uint64_t n;                                                                            \
            luxrt_write_full(2, #c "\n", sizeof(#c "\n") - 1, &n);                                 \
            return __LINE__;                                                                       \
        }                                                                                          \
    } while (0)
int32_t luxrt_hash_cases(const char *unused, uint64_t *outchecks) {
    (void)unused;
    uint64_t checks = 0;
    unsigned char text[80];
    for (int i = 0; i < 80; i++)
        text[i] = (unsigned char)i;
    for (int n = 0; n < 64; n++)
        CHECK(luxrt_siphash24(text, n, UINT64_C(0x0706050403020100),
                              UINT64_C(0x0f0e0d0c0b0a0908)) == siphash_vectors[n]);
    CHECK(luxrt_siphash24(0, 0, UINT64_C(0x0706050403020100), UINT64_C(0x0f0e0d0c0b0a0908)) ==
          siphash_vectors[0]);
    luxrt_heap a, b, d, tiny;
    CHECK(luxrt_heap_init(&a, UINT64_C(67108864), 4096) == 0);
    CHECK(luxrt_heap_init(&b, UINT64_C(67108864), 4096) == 0);
    CHECK(luxrt_heap_init(&d, UINT64_C(67108864), 4096) == 0);
    CHECK(luxrt_heap_init(&tiny, 4096, 4096) == 0);
    CHECK(luxrt_heap_seed_hash(&tiny, 1, 2) == -12 && tiny.segments == 0 && tiny.mapped_bytes == 0);
    CHECK(luxrt_heap_destroy(&tiny) == 0);
    CHECK(luxrt_heap_seed_hash(&a, UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)) ==
          0);
    CHECK(luxrt_heap_seed_hash(&b, 17, 29) == 0);
    luxrt_value ar[4] = {0}, br[2] = {0}, dr[1] = {0};
    luxrt_roots af, bf, df;
    CHECK(luxrt_roots_push(&a, &af, ar, 4) == 0 && luxrt_roots_push(&b, &bf, br, 2) == 0 &&
          luxrt_roots_push(&d, &df, dr, 1) == 0);
    CHECK(luxrt_map_new(&a, 0, &ar[0]) == 0);
    for (uint64_t i = 0; i < 4000; i++) {
        for (unsigned j = 0; j < 8; j++)
            text[j] = (unsigned char)("0123456789abcdef"[(i >> (4 * j)) & 15]);
        CHECK(luxrt_string_new(&a, text, 8, &ar[1]) == 0);
        CHECK(luxrt_string_new(&b, text, 8, &br[0]) == 0);
        CHECK(luxrt_string_new(&d, text, 8, &dr[0]) == 0);
        int32_t equal = 0;
        CHECK(luxrt_string_equal(ar[1], br[0], &equal) == 0 && equal);
        CHECK(luxrt_string_equal(ar[1], dr[0], &equal) == 0 && equal);
        uint64_t bytes, chars, ah, bh;
        const void *data;
        CHECK(luxrt_string_info(ar[1], &bytes, &chars, &data, &ah) == 0 && bytes == 8);
        CHECK(luxrt_string_info(br[0], &bytes, &chars, &data, &bh) == 0);
        CHECK(ah == luxrt_siphash24(text, 8, UINT64_C(0x0123456789abcdef),
                                    UINT64_C(0xfedcba9876543210)) &&
              ah != bh);
        CHECK(luxrt_value_int(&a, (int64_t)i, &ar[2]) == 0);
        CHECK(luxrt_map_set(&a, ar[0], ar[1], ar[2]) == 0);
        luxrt_value result = 0;
        CHECK(luxrt_map_get(ar[0], br[0], &result) == 0 && result == ar[2]);
        CHECK(luxrt_map_get(ar[0], dr[0], &result) == 0 && result == ar[2]);
        CHECK(luxrt_map_set(&a, ar[0], br[0], ar[2]) == -22);
        if (!(i % 3)) {
            CHECK(luxrt_map_delete(ar[0], br[0], &result) == 0 && result == ar[2]);
            CHECK(luxrt_map_set(&a, ar[0], ar[1], ar[2]) == 0);
        }
        if (!(i % 127)) {
            CHECK(luxrt_heap_collect(&a) == 0);
            CHECK(luxrt_heap_seed_hash(&a, 0, 0) == -16);
        }
    }
    CHECK(a.mapped_bytes > 65536);
    CHECK(luxrt_map_new(&a, LUXRT_MAP_NUMERIC_KEYS, &ar[3]) == 0);
    luxrt_value integer, floatzero, result;
    CHECK(luxrt_value_int(&a, 0, &integer) == 0);
    CHECK(luxrt_value_float_bits(&a, UINT64_C(0x8000000000000000), &ar[1]) == 0);
    floatzero = ar[1];
    CHECK(luxrt_map_set(&a, ar[3], integer, LUXRT_TRUE) == 0);
    CHECK(luxrt_map_get(ar[3], LUXRT_FALSE, &result) == 0 && result == LUXRT_TRUE);
    CHECK(luxrt_map_get(ar[3], floatzero, &result) == 0 && result == LUXRT_TRUE);
    CHECK(luxrt_map_set(&a, ar[3], floatzero, LUXRT_NONE) == 0);
    CHECK(luxrt_map_get(ar[3], integer, &result) == 0 && result == LUXRT_NONE);
    for (int i = 0; i < 4; i++)
        ar[i] = 0;
    for (int i = 0; i < 2; i++)
        br[i] = 0;
    dr[0] = 0;
    CHECK(luxrt_heap_collect(&a) == 0 && a.objects == 0);
    CHECK(luxrt_heap_seed_hash(&a, 0, 0) == 0);
    CHECK(luxrt_string_new(&a, "new", 3, &ar[0]) == 0);
    uint64_t bytes, chars, hash;
    const void *data;
    CHECK(luxrt_string_info(ar[0], &bytes, &chars, &data, &hash) == 0 &&
          hash == luxrt_siphash24("new", 3, 0, 0));
    CHECK(luxrt_roots_pop(&a, &af) == 0 && luxrt_roots_pop(&b, &bf) == 0 &&
          luxrt_roots_pop(&d, &df) == 0);
    CHECK(luxrt_heap_destroy(&a) == 0 && luxrt_heap_destroy(&b) == 0 &&
          luxrt_heap_destroy(&d) == 0);
    for (int i = 0; i < 20; i++) {
        CHECK(luxrt_heap_init_secure(&a, UINT64_C(67108864), 4096) == 0);
        CHECK(luxrt_string_new(&a, "fresh", 5, &ar[0]) == 0);
        uint64_t fresh;
        CHECK(luxrt_string_info(ar[0], &bytes, &chars, &data, &fresh) == 0);
        CHECK(luxrt_heap_init_secure(&b, UINT64_C(67108864), 4096) == 0);
        CHECK(luxrt_string_new(&b, "fresh", 5, &br[0]) == 0);
        uint64_t another;
        CHECK(luxrt_string_info(br[0], &bytes, &chars, &data, &another) == 0 && fresh != another);
        int32_t equal = 0;
        CHECK(luxrt_string_equal(ar[0], br[0], &equal) == 0 && equal);
        CHECK(luxrt_heap_destroy(&a) == 0 && luxrt_heap_destroy(&b) == 0);
    }
    *outchecks = checks;
    return 0;
}
