#ifndef LUXRT_MAP_H
#define LUXRT_MAP_H
#include "collections.h"
/* Insertion-ordered immutable scalar keys: Int, Bool, None, Float, String.
 * Default keys compare by type (Float +0/-0 compare equal). With NUMERIC_KEYS,
 * Bool/Int/integral Float share a key, without losing large Int precision.
 * NaNs compare only by object identity. New insertion follows existing order;
 * replacing preserves the original key and position, delete/reinsert moves
 * to the end. Missing keys/end iteration: -2; bad type/flags: -22.
 * Do not mutate a map during cursor iteration. Hashing is currently not a
 * cryptographic defense against adversarial collision flooding.
 */
#define LUXRT_MAP_NUMERIC_KEYS UINT64_C(1)
int32_t luxrt_map_new(luxrt_heap *, uint64_t flags, luxrt_value *out);
int32_t luxrt_map_set(luxrt_heap *, luxrt_value map, luxrt_value key, luxrt_value value);
int32_t luxrt_map_get(luxrt_value map, luxrt_value key, luxrt_value *out);
int32_t luxrt_map_delete(luxrt_value map, luxrt_value key, luxrt_value *old);
int32_t luxrt_map_length(luxrt_value map, uint64_t *out);
int32_t luxrt_map_next(luxrt_value map, uint64_t *cursor, luxrt_value *key, luxrt_value *value);
int32_t luxrt_map_clear(luxrt_value map);
int32_t luxrt_set_new(luxrt_heap *, uint64_t flags, luxrt_value *out);
int32_t luxrt_set_add(luxrt_heap *, luxrt_value set, luxrt_value key);
int32_t luxrt_set_contains(luxrt_value set, luxrt_value key, int32_t *out);
int32_t luxrt_set_remove(luxrt_value set, luxrt_value key);
#endif
