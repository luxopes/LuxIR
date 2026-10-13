#include "numeric.h"
#include "collections.h"
#include "float64.h"
#include "integer.h"
int32_t luxrt_string_to_int(luxrt_heap *h, luxrt_value v, luxrt_value *out) {
    uint64_t bytes, chars, hash;
    const void *data;
    int64_t number;
    int32_t status = luxrt_string_info(v, &bytes, &chars, &data, &hash);
    if (status)
        return status;
    status = luxrt_parse_i64(data, bytes, &number);
    return status ? status : luxrt_value_int(h, number, out);
}
int32_t luxrt_string_to_float(luxrt_heap *h, luxrt_value v, luxrt_value *out) {
    uint64_t bytes, chars, hash, bits;
    const void *data;
    int32_t status = luxrt_string_info(v, &bytes, &chars, &data, &hash);
    if (status)
        return status;
    status = luxrt_parse_float64(data, bytes, &bits);
    return status ? status : luxrt_value_float_bits(h, bits, out);
}
int32_t luxrt_int_to_string(luxrt_heap *h, luxrt_value v, luxrt_value *out) {
    int64_t number;
    unsigned char text[32];
    int32_t status = luxrt_value_get_int(v, &number);
    if (status)
        return status;
    int64_t bytes = luxrt_format_i64(number, text, sizeof text);
    return bytes < 0 ? (int32_t)bytes : luxrt_string_new(h, text, (uint64_t)bytes, out);
}
int32_t luxrt_float_to_string(luxrt_heap *h, luxrt_value v, luxrt_value *out) {
    uint64_t bits;
    unsigned char text[32];
    int32_t status = luxrt_value_get_float_bits(v, &bits);
    if (status)
        return status;
    int64_t bytes = luxrt_format_float64(bits, text, sizeof text);
    return bytes < 0 ? (int32_t)bytes : luxrt_string_new(h, text, (uint64_t)bytes, out);
}
