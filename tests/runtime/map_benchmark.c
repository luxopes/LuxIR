#define _POSIX_C_SOURCE 200809L
#include "../../runtime/map.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef LUXRT_HASH_SEEDED
#define LUXRT_HASH_SEEDED 0
#endif
#define KEYS 4096
#define CALLS 1000000
static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        abort();
    return t.tv_sec + t.tv_nsec * 1e-9;
}
int main(void) {
    luxrt_heap h;
    luxrt_value roots[2] = {0}, *keys = calloc(KEYS, sizeof *keys);
    luxrt_roots frame, key_frame;
    if (!keys || luxrt_heap_init(&h, 64 * 1024 * 1024, 4096) ||
        (LUXRT_HASH_SEEDED && luxrt_heap_seed_hash(&h, 12345, 67890)) ||
        luxrt_roots_push(&h, &frame, roots, 2) || luxrt_roots_push(&h, &key_frame, keys, KEYS) ||
        luxrt_map_new(&h, 0, roots))
        return 1;
    for (unsigned i = 0; i < KEYS; i++) {
        char text[32];
        int n = snprintf(text, sizeof text, "station_%u", i);
        if (luxrt_string_new(&h, text, (uint64_t)n, keys + i) ||
            luxrt_string_new(&h, text, (uint64_t)n, roots + 1) ||
            luxrt_map_set(&h, roots[0], roots[1], ((uint64_t)i << 3) | 1))
            abort();
    }
    if (luxrt_heap_collect(&h))
        abort();
    uint64_t expected = 0;
    for (unsigned i = 0; i < CALLS; i++)
        expected += ((uint64_t)(i & (KEYS - 1)) << 3) | 1;
    double timings[7];
    for (unsigned trial = 0; trial < 8; trial++) {
        uint64_t checksum = 0;
        double start = now();
        for (unsigned i = 0; i < CALLS; i++) {
            luxrt_value value;
            if (luxrt_map_get(roots[0], keys[i & (KEYS - 1)], &value))
                abort();
            checksum += value;
        }
        double elapsed = now() - start;
        if (checksum != expected)
            abort();
        if (trial)
            timings[trial - 1] = elapsed * 1e9 / CALLS;
    }
    for (unsigned i = 0; i < 7; i++)
        for (unsigned j = i + 1; j < 7; j++)
            if (timings[j] < timings[i]) {
                double t = timings[i];
                timings[i] = timings[j];
                timings[j] = t;
            }
    printf("{\"keys\":%d,\"lookups_per_trial\":%d,\"trials\":7,\"median_ns_per_lookup\":%.6f,"
           "\"distinct_equal_string_objects\":true}\n",
           KEYS, CALLS, timings[3]);
    if (luxrt_roots_pop(&h, &key_frame) || luxrt_roots_pop(&h, &frame) || luxrt_heap_destroy(&h))
        abort();
    free(keys);
    return 0;
}
