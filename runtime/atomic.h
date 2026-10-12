#ifndef LUXRT_ATOMIC_H
#define LUXRT_ATOMIC_H
#include <stdint.h>
/* Naturally aligned atomic 32/64-bit storage; all operations sequentially
 * consistent. Returns the old value for add/swap/cas. CAS writes desired
 * only if the old value equals expected. No global locks, no hidden allocs.
 * This is external runtime API, not new LuxIR instructions. */
#define LUXRT_ATOMIC_DECL(B,T) \
 T luxrt_atomic_load##B(const void *p); \
 void luxrt_atomic_store##B(void *p,T value); \
 T luxrt_atomic_add##B(void *p,T value); \
 T luxrt_atomic_swap##B(void *p,T value); \
 T luxrt_atomic_cas##B(void *p,T expected,T desired);
LUXRT_ATOMIC_DECL(32,uint32_t)
LUXRT_ATOMIC_DECL(64,uint64_t)
void luxrt_atomic_fence(void);
#endif
