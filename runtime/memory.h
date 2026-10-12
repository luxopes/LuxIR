#ifndef LUXRT_MEMORY_H
#define LUXRT_MEMORY_H
#include <stddef.h>
#include <stdint.h>
/* Raw byte ABI: valid spans supplied by caller; zero length touches no memory. */
void *luxrt_memcpy(void *dst, const void *src, uint64_t n);
void *luxrt_memmove(void *dst, const void *src, uint64_t n);
void *luxrt_memset(void *dst, uint32_t byte, uint64_t n);
int32_t luxrt_memcmp(const void *left, const void *right, uint64_t n);
void *luxrt_memchr(const void *data, uint32_t byte, uint64_t n);
uint32_t luxrt_hash_bytes(const void *data, uint64_t n);
int32_t luxrt_utf8_valid(const void *data, uint64_t n);
#endif
