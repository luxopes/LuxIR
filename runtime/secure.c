#include "managed.h"
#include "memory.h"
#include "os.h"
/* Separate optional initializer: ordinary managed core has no entropy dependency. */
int32_t luxrt_heap_init_secure(luxrt_heap *heap, uint64_t maximum, uint64_t page) {
    luxrt_heap temporary;
    int32_t status = luxrt_heap_init(&temporary, maximum, page);
    if (status)
        return status;
    uint64_t key[2] = {0, 0}, at = 0;
    while (at < sizeof key) {
        int64_t count = luxrt_os_random((unsigned char *)key + at, sizeof key - at, 0);
        if (count == -4)
            continue;
        if (count <= 0 || (uint64_t)count > sizeof key - at) {
            status = count < 0 ? (int32_t)count : -5;
            luxrt_memset(key, 0, sizeof key);
            return status;
        }
        at += (uint64_t)count;
    }
    status = luxrt_heap_seed_hash(&temporary, key[0], key[1]);
    luxrt_memset(key, 0, sizeof key);
    if (status) {
        luxrt_heap_destroy(&temporary);
        return status;
    }
    luxrt_memcpy(heap, &temporary, sizeof temporary);
    luxrt_memset(&temporary, 0, sizeof temporary);
    return 0;
}
