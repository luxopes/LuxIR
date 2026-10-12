#ifndef LUXRT_INTEGER_H
#define LUXRT_INTEGER_H
#include <stdint.h>
/* Decimal only; 0 success, -22 syntax, -34 overflow. Output unchanged on error. */
int32_t luxrt_parse_i64(const void *text,uint64_t length,int64_t *output);
/* Returns bytes written (no NUL); -28 insufficient capacity, no writes. */
int64_t luxrt_format_i64(int64_t value,void *buffer,uint64_t capacity);
#endif
