#include "hash.h"
#define ROT(x, n) (((x) << (n)) | ((x) >> (64 - (n))))
#define ROUND()                                                                                    \
    do {                                                                                           \
        v0 += v1;                                                                                  \
        v1 = ROT(v1, 13);                                                                          \
        v1 ^= v0;                                                                                  \
        v0 = ROT(v0, 32);                                                                          \
        v2 += v3;                                                                                  \
        v3 = ROT(v3, 16);                                                                          \
        v3 ^= v2;                                                                                  \
        v0 += v3;                                                                                  \
        v3 = ROT(v3, 21);                                                                          \
        v3 ^= v0;                                                                                  \
        v2 += v1;                                                                                  \
        v1 = ROT(v1, 17);                                                                          \
        v1 ^= v2;                                                                                  \
        v2 = ROT(v2, 32);                                                                          \
    } while (0)
uint64_t luxrt_siphash24(const void *data, uint64_t size, uint64_t key0, uint64_t key1) {
    const unsigned char *p = data;
    uint64_t v0 = UINT64_C(0x736f6d6570736575) ^ key0, v1 = UINT64_C(0x646f72616e646f6d) ^ key1;
    uint64_t v2 = UINT64_C(0x6c7967656e657261) ^ key0, v3 = UINT64_C(0x7465646279746573) ^ key1;
    uint64_t at = 0;
    while (size - at >= 8) {
        uint64_t m = 0;
        for (uint64_t j = 0; j < 8; j++)
            m |= (uint64_t)p[at + j] << (j * 8);
        v3 ^= m;
        ROUND();
        ROUND();
        v0 ^= m;
        at += 8;
    }
    uint64_t tail = (size & 255) << 56;
    for (uint64_t j = 0; j < size - at; j++)
        tail |= (uint64_t)p[at + j] << (j * 8);
    v3 ^= tail;
    ROUND();
    ROUND();
    v0 ^= tail;
    v2 ^= 255;
    ROUND();
    ROUND();
    ROUND();
    ROUND();
    return v0 ^ v1 ^ v2 ^ v3;
}
