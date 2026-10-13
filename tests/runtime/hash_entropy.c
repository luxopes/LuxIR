#include "../../runtime/collections.h"
#include "../../runtime/hash.h"
#include "../../runtime/managed.h"
#include "../../runtime/memory.h"
#include <stdio.h>
static uint64_t mode, calls, offset, checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "entropy test line %d\n", __LINE__);                                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int64_t luxrt_os_random(void *data, uint64_t size, int64_t flags) {
    (void)flags;
    calls++;
    if (mode == 1)
        return -38;
    if (mode == 2)
        return 0;
    if (mode == 3 && offset)
        return -1;
    if (mode == 4)
        return (int64_t)size + 1;
    if (calls == 1)
        return -4;
    if (size > 3)
        size = 3;
    unsigned char *p = data;
    for (uint64_t i = 0; i < size; i++)
        p[i] = (unsigned char)offset++;
    return (int64_t)size;
}
int main(void) {
    luxrt_heap h;
    unsigned char original[sizeof h];
    for (mode = 0; mode < 5; mode++) {
        calls = 0;
        offset = 0;
        luxrt_memset(&h, 0xa5, sizeof h);
        luxrt_memcpy(original, &h, sizeof h);
        int32_t s = luxrt_heap_init_secure(&h, 67108864, 4096);
        if (!mode) {
            CHECK(s == 0 && offset == 16 && calls > 6);
            luxrt_value string;
            CHECK(luxrt_string_new(&h, "seed", 4, &string) == 0);
            uint64_t n, chars, hash;
            const void *data;
            CHECK(luxrt_string_info(string, &n, &chars, &data, &hash) == 0 &&
                  hash == luxrt_siphash24("seed", 4, UINT64_C(0x0706050403020100),
                                          UINT64_C(0x0f0e0d0c0b0a0908)));
            CHECK(luxrt_heap_destroy(&h) == 0);
        } else {
            CHECK(s == (mode == 1 ? -38 : mode == 3 ? -1 : -5));
            CHECK(!luxrt_memcmp(original, &h, sizeof h));
        }
    }
    mode = 0;
    calls = 0;
    offset = 0;
    luxrt_memset(&h, 0xa5, sizeof h);
    luxrt_memcpy(original, &h, sizeof h);
    CHECK(luxrt_heap_init_secure(&h, 4096, 4096) == -12 && offset == 16);
    CHECK(!luxrt_memcmp(original, &h, sizeof h));
    calls = 0;
    offset = 0;
    CHECK(luxrt_heap_init_secure(&h, 67108864, 65) == -22 && calls == 0);
    CHECK(!luxrt_memcmp(original, &h, sizeof h));
    printf("%lu entropy fault checks passed\n", checks);
    return 0;
}
