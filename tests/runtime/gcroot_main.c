#include <stdint.h>
#include <stdio.h>
int32_t luxrt_gcroot_cases(uint64_t *);
extern uint64_t test_mapped_bytes;
int main(void) {
    uint64_t n=0;
    int32_t status = luxrt_gcroot_cases(&n);
    if (status || test_mapped_bytes) { fprintf(stderr,"GC root failed line %d; mapped %lu\n",status,test_mapped_bytes);return 1; }
    printf("%lu automatic shadow-root checks passed\n",n);
    return 0;
}
