#include "float64.h"
#include "integer.h"
#include "memory.h"
#include <stddef.h>
#define RYU_ONLY_64_BIT_OPS
#define NDEBUG
#define memcpy luxrt_memcpy
#include "vendor/ryu/d2s.c"
#include "vendor/ryu/s2d.c"
#undef memcpy
/* Decimal summary retains all rounding-relevant digits. Every Float64
 * midpoint has fewer than 800 significant decimal digits. Tail nonzero
 * preserves strict ordering when input extends beyond that bound. */
typedef struct {
    unsigned char digits[800];
    int64_t count, exponent;
    int32_t saved, sticky, negative;
} decimal;
static int32_t digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int32_t parse_decimal(const unsigned char *p, uint64_t n, decimal *d) {
    if (n > (uint64_t)INT64_MAX / 4) return -34;
    if (!n || !p) return -22;
    uint64_t i = 0;
    d->count = 0;
    d->saved = 0;
    d->sticky = 0;
    d->negative = 0;
    d->exponent = 0;
    if (p[i] == '+' || p[i] == '-') {
        d->negative = p[i] == '-';
        i++;
    }
    int32_t any = 0, point = 0;
    int64_t fraction = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c == '.') {
            if (point)
                return -22;
            point = 1;
            i++;
            continue;
        }
        if (!digit(c))
            break;
        any = 1;
        if (point)
            fraction++;
        if (c != '0' || d->count) {
            d->count++;
            if (d->saved < 800)
                d->digits[d->saved++] = c;
            else if (c != '0')
                d->sticky = 1;
        }
        i++;
    }
    if (!any)
        return -22;
    int64_t exponent = 0, bound = (int64_t)n + 4096;
    int32_t negative = 0;
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) {
            negative = p[i] == '-';
            i++;
        }
        uint64_t first = i;
        while (i < n && digit(p[i])) {
            int64_t x = p[i] - '0';
            exponent = exponent > (bound - x) / 10 ? bound : exponent * 10 + x;
            i++;
        }
        if (i == first)
            return -22;
    }
    if (i != n)
        return -22;
    d->exponent = (negative ? -exponent : exponent) - fraction;
    return 0;
}
/* Compare normalized input with midpoint between adjacent nonnegative bits. */
static int32_t compare_midpoint(const decimal *d, uint64_t bits) {
    uint64_t mantissa = bits & UINT64_C(0xfffffffffffff);
    int32_t exponent = (int32_t)(bits >> 52);
    if (exponent) {
        mantissa |= UINT64_C(0x10000000000000);
        exponent -= 1075;
    } else
        exponent = -1074;
    mantissa = mantissa * 2 + 1;
    exponent--;
    uint32_t limbs[96] = {0};
    uint32_t count = 0;
    while (mantissa) {
        limbs[count++] = (uint32_t)(mantissa % 1000000000);
        mantissa /= 1000000000;
    }
    int32_t rounds = exponent < 0 ? -exponent : exponent;
    uint64_t factor = exponent < 0 ? 5 : 2;
    for (int32_t r = 0; r < rounds; r++) {
        uint64_t carry = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t v = (uint64_t)limbs[i] * factor + carry;
            limbs[i] = (uint32_t)(v % 1000000000);
            carry = v / 1000000000;
        }
        if (carry)
            limbs[count++] = (uint32_t)carry;
    }
    unsigned char threshold[864];
    uint32_t length = 0;
    for (uint32_t i = count; i > 0; i--) {
        uint32_t v = limbs[i - 1];
        unsigned char block[9];
        uint32_t size = 0;
        do {
            block[size++] = (unsigned char)('0' + v % 10);
            v /= 10;
        } while (v);
        if (i != count)
            while (size < 9)
                block[size++] = '0';
        while (size)
            threshold[length++] = block[--size];
    }
    int64_t threshold_exp = exponent < 0 ? exponent : 0;
    int64_t order = d->count + d->exponent, other = (int64_t)length + threshold_exp;
    if (order != other)
        return order < other ? -1 : 1;
    uint64_t compare = d->count > (int64_t)length ? (uint64_t)d->count : length;
    if (compare > 800)
        compare = 800;
    for (uint64_t i = 0; i < compare; i++) {
        unsigned char a = i < (uint64_t)d->saved ? d->digits[i] : '0',
                      b = i < length ? threshold[i] : '0';
        if (a != b)
            return a < b ? -1 : 1;
    }
    return d->sticky ? 1 : 0;
}
int32_t luxrt_parse_float64(const void *text, uint64_t n, uint64_t *out) {
    const unsigned char *p = text;
    if (n > (uint64_t)INT64_MAX / 4) return -34;
    if (!p || !n)
        return -22;
    uint64_t start = 0, sign = 0;
    if (p[0] == '+' || p[0] == '-') {
        sign = p[0] == '-' ? UINT64_C(0x8000000000000000) : 0;
        start = 1;
    }
    if ((n - start == 3 && luxrt_memcmp(p + start, "inf", 3) == 0) ||
        (n - start == 8 && luxrt_memcmp(p + start, "infinity", 8) == 0)) {
        *out = sign | UINT64_C(0x7ff0000000000000);
        return 0;
    }
    if (n - start == 3 && luxrt_memcmp(p + start, "nan", 3) == 0) {
        *out = sign | UINT64_C(0x7ff8000000000000);
        return 0;
    }
    decimal d;
    int32_t status = parse_decimal(p, n, &d);
    if (status)
        return status;
    if (!d.count) {
        *out = sign;
        return 0;
    }
    int64_t order = d.count + d.exponent;
    if (order > 309) {
        *out = sign | UINT64_C(0x7ff0000000000000);
        return 0;
    }
    if (order < -323) {
        *out = sign;
        return 0;
    }
    char short_text[48];
    uint32_t digits = d.saved < 17 ? (uint32_t)d.saved : 17;
    for (uint32_t i = 0; i < digits; i++)
        short_text[i] = (char)d.digits[i];
    short_text[digits] = 'e';
    int64_t e = d.exponent + d.count - digits;
    int64_t wrote = luxrt_format_i64(e, short_text + digits + 1, 47 - digits);
    if (wrote < 0)
        return -34;
    double value;
    status = s2d_n(short_text, (int)(digits + 1 + wrote), &value);
    if (status != SUCCESS)
        return -34;
    uint64_t bits;
    luxrt_memcpy(&bits, &value, 8);
    if (d.count > 17) {
        /* Truncated 17 digits are a close hint. Exact midpoint comparison also
         * handles halfway ties, max-finite overflow and the zero/subnormal edge. */
        for (;;) {
            if (bits < UINT64_C(0x7ff0000000000000)) {
                int32_t c = compare_midpoint(&d, bits);
                if (c > 0 || (c == 0 && (bits & 1))) {
                    bits++;
                    continue;
                }
            }
            if (bits) {
                int32_t c = compare_midpoint(&d, bits - 1);
                if (c < 0 || (c == 0 && (bits & 1))) {
                    bits--;
                    continue;
                }
            }
            break;
        }
    }
    *out = sign | bits;
    return 0;
}
int64_t luxrt_format_float64(uint64_t bits, void *output, uint64_t capacity) {
    char result[32];
    uint32_t size = 0;
    int32_t negative = (bits >> 63) != 0;
    uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
    if (magnitude > UINT64_C(0x7ff0000000000000)) {
        luxrt_memcpy(result, "nan", 3);
        size = 3;
    } else if (magnitude == UINT64_C(0x7ff0000000000000)) {
        if (negative)
            result[size++] = '-';
        luxrt_memcpy(result + size, "inf", 3);
        size += 3;
    } else if (!magnitude) {
        if (negative)
            result[size++] = '-';
        luxrt_memcpy(result + size, "0.0", 3);
        size += 3;
    } else {
        double value;
        luxrt_memcpy(&value, &magnitude, 8);
        char scientific[32];
        int length = d2s_buffered_n(value, scientific);
        char digits[17];
        int n = 0, pos = 0;
        while (pos < length && scientific[pos] != 'E') {
            if (scientific[pos] != '.')
                digits[n++] = scientific[pos];
            pos++;
        }
        pos++;
        int exponent = 0, sign = 1;
        if (scientific[pos] == '-') {
            sign = -1;
            pos++;
        }
        while (pos < length)
            exponent = exponent * 10 + scientific[pos++] - '0';
        exponent *= sign;
        if (negative)
            result[size++] = '-';
        if (exponent >= -4 && exponent < 16) {
            int point = exponent + 1;
            if (point <= 0) {
                result[size++] = '0';
                result[size++] = '.';
                for (int i = 0; i < -point; i++)
                    result[size++] = '0';
                for (int i = 0; i < n; i++)
                    result[size++] = digits[i];
            } else {
                for (int i = 0; i < point; i++)
                    result[size++] = i < n ? digits[i] : '0';
                result[size++] = '.';
                if (point < n)
                    for (int i = point; i < n; i++)
                        result[size++] = digits[i];
                else
                    result[size++] = '0';
            }
        } else {
            result[size++] = digits[0];
            if (n > 1) {
                result[size++] = '.';
                for (int i = 1; i < n; i++)
                    result[size++] = digits[i];
            }
            result[size++] = 'e';
            result[size++] = exponent < 0 ? '-' : '+';
            int abs = exponent < 0 ? -exponent : exponent;
            if (abs >= 100)
                result[size++] = (char)('0' + abs / 100);
            result[size++] = (char)('0' + abs / 10 % 10);
            result[size++] = (char)('0' + abs % 10);
        }
    }
    if (size > capacity)
        return -28;
    luxrt_memcpy(output, result, size);
    return size;
}
