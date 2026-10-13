#include <stdint.h>
int32_t invoke_probe(uint64_t x, uint64_t *out, uint64_t *count) {
    ++*count;
    uint32_t selector = x & 7;
    if (selector) return (selector & 1) ? -(int32_t)selector : (int32_t)selector;
    *out = x ^ UINT64_C(0x98f91abe66543127);
    return 0;
}
