#include "atomic.h"
#include <stdatomic.h>
#define IMPL(B,T) \
 T luxrt_atomic_load##B(const void *p){return atomic_load_explicit((const _Atomic T*)p,memory_order_seq_cst);} \
 void luxrt_atomic_store##B(void *p,T v){atomic_store_explicit((_Atomic T*)p,v,memory_order_seq_cst);} \
 T luxrt_atomic_add##B(void *p,T v){return atomic_fetch_add_explicit((_Atomic T*)p,v,memory_order_seq_cst);} \
 T luxrt_atomic_swap##B(void *p,T v){return atomic_exchange_explicit((_Atomic T*)p,v,memory_order_seq_cst);} \
 T luxrt_atomic_cas##B(void *p,T expected,T desired){atomic_compare_exchange_strong_explicit((_Atomic T*)p,&expected,desired,memory_order_seq_cst,memory_order_seq_cst);return expected;}
IMPL(32,uint32_t)
IMPL(64,uint64_t)
void luxrt_atomic_fence(void){atomic_thread_fence(memory_order_seq_cst);}
