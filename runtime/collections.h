#ifndef LUXRT_COLLECTIONS_H
#define LUXRT_COLLECTIONS_H
#include "managed.h"
/* Managed immutable UTF-8 strings, dynamic Value lists and unboxed arrays.
 * No language-specific coercion; indices are unsigned and bounds-checked.
 * Statuses: 0, -22 type/parameter, -34 range, -84 invalid UTF-8, -12 OOM.
 * Failures preserve outputs and logical contents (GC itself may still run).
 */
#define LUXRT_INT_ARRAY_TYPE UINT64_C(22)
#define LUXRT_FLOAT_ARRAY_TYPE UINT64_C(23)
#define LUXRT_BYTE_ARRAY_TYPE UINT64_C(24)
int32_t luxrt_string_new(luxrt_heap *, const void *, uint64_t, luxrt_value *);
int32_t luxrt_string_info(luxrt_value, uint64_t *bytes, uint64_t *characters, const void **data,
                          uint64_t *hash);
int32_t luxrt_string_concat(luxrt_heap *, luxrt_value, luxrt_value, luxrt_value *);
int32_t luxrt_string_slice(luxrt_heap *, luxrt_value, uint64_t begin, uint64_t end, luxrt_value *);
int32_t luxrt_string_equal(luxrt_value, luxrt_value, int32_t *);
int32_t luxrt_string_compare(luxrt_value, luxrt_value, int32_t *);
int32_t luxrt_string_split(luxrt_heap *, luxrt_value, luxrt_value separator, luxrt_value *);
int32_t luxrt_values_new(luxrt_heap *, uint64_t capacity, luxrt_value *);
int32_t luxrt_values_append(luxrt_heap *, luxrt_value, luxrt_value);
int32_t luxrt_values_get(luxrt_value, uint64_t, luxrt_value *);
int32_t luxrt_values_set(luxrt_heap *, luxrt_value, uint64_t, luxrt_value);
int32_t luxrt_values_pop(luxrt_value, luxrt_value *);
int32_t luxrt_values_clear(luxrt_value);
int32_t luxrt_vector_length(luxrt_value, uint64_t *);
int32_t luxrt_array_new(luxrt_heap *, uint64_t kind, uint64_t length, luxrt_value *);
int32_t luxrt_array_append(luxrt_heap *, luxrt_value, uint64_t bits);
int32_t luxrt_array_get(luxrt_value, uint64_t index, uint64_t *bits);
int32_t luxrt_array_set(luxrt_value, uint64_t index, uint64_t bits);
int32_t luxrt_array_pop(luxrt_value, uint64_t *bits);
#endif
