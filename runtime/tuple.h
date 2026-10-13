#ifndef LUXRT_TUPLE_H
#define LUXRT_TUPLE_H
#include "managed.h"
#define LUXRT_TUPLE_TYPE UINT64_C(25)
/* Immutable sequence of Values. Tuples can contain mutable objects; their
 * references are immutable, not recursively frozen. No automatic equality,
 * hashing or language-specific index coercion. A borrowed input span must
 * remain valid through collection; root its owner if it is managed memory.
 * All operations preserve outputs on failure. -22 type/owner, -34 range.
 */
int32_t luxrt_tuple_new(luxrt_heap *, const luxrt_value *, uint64_t count, luxrt_value *);
int32_t luxrt_tuple_length(luxrt_value, uint64_t *);
int32_t luxrt_tuple_get(luxrt_value, uint64_t index, luxrt_value *);
int32_t luxrt_tuple_slice(luxrt_heap *, luxrt_value, uint64_t begin, uint64_t end, luxrt_value *);
int32_t luxrt_tuple_concat(luxrt_heap *, luxrt_value, luxrt_value, luxrt_value *);
int32_t luxrt_tuple_from_values(luxrt_heap *, luxrt_value, luxrt_value *);
#endif
