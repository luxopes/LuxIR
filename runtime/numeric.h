#ifndef LUXRT_NUMERIC_H
#define LUXRT_NUMERIC_H
#include "managed.h"
/* Managed adapters over the strict raw decimal conversion API. No implicit
 * Bool/Int/Float coercion. Root live Values before calls that may allocate.
 * Errors preserve *out: -22 type/syntax, -34 integer overflow, -12 OOM.
 */
int32_t luxrt_string_to_int(luxrt_heap *, luxrt_value, luxrt_value *out);
int32_t luxrt_string_to_float(luxrt_heap *, luxrt_value, luxrt_value *out);
int32_t luxrt_int_to_string(luxrt_heap *, luxrt_value, luxrt_value *out);
int32_t luxrt_float_to_string(luxrt_heap *, luxrt_value, luxrt_value *out);
#endif
