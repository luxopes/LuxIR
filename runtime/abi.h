#ifndef LUXRT_ABI_H
#define LUXRT_ABI_H
#include <stdint.h>
/* Luxrt ABI 1: little-endian 64-bit scalars, single-owner explicit GC roots.
 * Use size queries for caller-owned contexts; use APIs instead of accessing
 * private object/heap fields. No automatic stack maps or shared-heap threads.
 * The retained object relocation requires this revision's guard at link time.
 */
extern const uint64_t luxrt_abi_v1_le64_explicit[4];
uint64_t luxrt_abi_revision(void);
uint64_t luxrt_heap_bytes(void);
uint64_t luxrt_roots_bytes(void);
uint64_t luxrt_descriptor_bytes(void);
#endif
