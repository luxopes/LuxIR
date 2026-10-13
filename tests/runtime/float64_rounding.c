#include "../../runtime/float64.h"
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    int saved = fegetround();
    int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    unsigned checks = 0;
    const char *input[] = {"1.00000000000000011102230246251565404236316680908203125",
                           "1.000000000000000111022302462515654042363166809082031251",
                           "-1.00000000000000011102230246251565404236316680908203125",
                           "2.4703282292062327e-324", "2.4703282292062328e-324"};
    uint64_t expected[] = {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000001),
                           UINT64_C(0xbff0000000000000), 0, 1};
    for (unsigned m = 0; m < 4; m++) {
        if (fesetround(modes[m]))
            return 1;
        for (unsigned i = 0; i < 5; i++) {
            uint64_t bits = 42;
            if (luxrt_parse_float64(input[i], strlen(input[i]), &bits) || bits != expected[i])
                return 2;
            checks++;
        }
        char output[32];
        int64_t n = luxrt_format_float64(UINT64_C(0x3ff0000000000001), output, 32);
        if (n != 18 || memcmp(output, "1.0000000000000002", 18))
            return 3;
        checks++;
    }
    if (fesetround(saved))
        return 4;
    printf("%u rounding-mode checks passed\n", checks);
    return 0;
}
