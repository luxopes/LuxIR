#include <stdint.h>
#include <stdio.h>
int32_t luxrt_extended_cases(uint64_t *);
extern uint64_t test_mapped_bytes;
int main(void) {
    uint64_t checks = 0;
    int32_t failure = luxrt_extended_cases(&checks);
    if (failure || test_mapped_bytes) {
        fprintf(stderr, "failed at line %d, mapped bytes %lu\n", failure, test_mapped_bytes);
        return 1;
    }
    printf("%lu managed numeric/tuple checks passed\n", checks);
    return 0;
}
