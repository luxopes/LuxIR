#ifndef LUXRT_CHECKED_H
#define LUXRT_CHECKED_H
#include <stdint.h>
/* Status: 0 success, -34 overflow, -33 division by zero; no store on error. */
int32_t luxrt_checked_add(int64_t a,int64_t b,int64_t *output);
int32_t luxrt_checked_sub(int64_t a,int64_t b,int64_t *output);
int32_t luxrt_checked_mul(int64_t a,int64_t b,int64_t *output);
int32_t luxrt_checked_floordiv(int64_t a,int64_t b,int64_t *output);
#endif
