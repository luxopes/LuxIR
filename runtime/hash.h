#ifndef LUXRT_HASH_H
#define LUXRT_HASH_H
#include <stdint.h>
/* SipHash-2-4, 64-bit result, 128-bit key supplied as two little-endian words.
 * Valid readable span is required; NULL is allowed for size zero. No allocation.
 * Hash-flood protection requires an unpredictable secret key; this is not a
 * password hash, encryption function, or arbitrary-length authentication tag.
 */
uint64_t luxrt_siphash24(const void *data, uint64_t size, uint64_t key0, uint64_t key1);
#endif
