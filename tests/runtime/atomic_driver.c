#define _GNU_SOURCE
#include "../../runtime/atomic.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static _Atomic uint64_t counter64;
static _Atomic uint32_t counter32,x,y;
static uint32_t rx,ry;
static pthread_barrier_t barrier;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
static void *increment(void *unused){(void)unused;for(unsigned i=0;i<100000;i++){luxrt_atomic_add64(&counter64,1);luxrt_atomic_add32(&counter32,1);}return NULL;}
static void *ordering(void *arg){uintptr_t side=(uintptr_t)arg;for(unsigned i=0;i<2000;i++){
 pthread_barrier_wait(&barrier);
 if(side){luxrt_atomic_store32(&x,1);rx=luxrt_atomic_load32(&y);}else {luxrt_atomic_store32(&y,1);ry=luxrt_atomic_load32(&x);}
 pthread_barrier_wait(&barrier);
}return NULL;}
#ifdef LUXIR_ATOMICS
extern uint64_t atomic_barrier_check(void*),atomic_store_barrier_check(void*),atomic_global_add(void);
extern void atomic_drop_result(void*),atomic_drop_cas_result(void*);
extern void *atomic_cas_worker(void*);
extern uint64_t atomic_register_stress(void*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
#endif
int main(void){unsigned checks=0;_Atomic uint64_t a=UINT64_MAX;_Atomic uint32_t b=UINT32_MAX;
 CHECK(luxrt_atomic_add64(&a,1)==UINT64_MAX);CHECK(luxrt_atomic_load64(&a)==0);
 CHECK(luxrt_atomic_add32(&b,1)==UINT32_MAX);CHECK(luxrt_atomic_load32(&b)==0);
 CHECK(luxrt_atomic_cas64(&a,1,9)==0);CHECK(luxrt_atomic_load64(&a)==0);
 CHECK(luxrt_atomic_cas64(&a,0,UINT64_MAX)==0);CHECK(luxrt_atomic_swap64(&a,17)==UINT64_MAX);CHECK(luxrt_atomic_load64(&a)==17);
 CHECK(luxrt_atomic_cas32(&b,1,9)==0);CHECK(luxrt_atomic_load32(&b)==0);
 CHECK(luxrt_atomic_cas32(&b,0,UINT32_MAX)==0);CHECK(luxrt_atomic_swap32(&b,17)==UINT32_MAX);CHECK(luxrt_atomic_load32(&b)==17);
 luxrt_atomic_store64(&a,42);luxrt_atomic_store32(&b,42);luxrt_atomic_fence();CHECK(luxrt_atomic_load64(&a)==42);CHECK(luxrt_atomic_load32(&b)==42);
 pthread_t threads[4];for(unsigned i=0;i<4;i++)CHECK(pthread_create(&threads[i],NULL,increment,NULL)==0);
 for(unsigned i=0;i<4;i++)CHECK(pthread_join(threads[i],NULL)==0);
 CHECK(luxrt_atomic_load64(&counter64)==400000);CHECK(luxrt_atomic_load32(&counter32)==400000);
 CHECK(pthread_barrier_init(&barrier,NULL,3)==0);CHECK(pthread_create(&threads[0],NULL,ordering,(void*)0)==0);CHECK(pthread_create(&threads[1],NULL,ordering,(void*)1)==0);
 for(unsigned i=0;i<2000;i++){luxrt_atomic_store32(&x,0);luxrt_atomic_store32(&y,0);pthread_barrier_wait(&barrier);pthread_barrier_wait(&barrier);CHECK(rx!=0||ry!=0);}
 CHECK(pthread_join(threads[0],NULL)==0);CHECK(pthread_join(threads[1],NULL)==0);CHECK(pthread_barrier_destroy(&barrier)==0);
#ifdef LUXIR_ATOMICS
 CHECK(atomic_barrier_check(&a)==1);CHECK(atomic_store_barrier_check(&a)==40);
 atomic_drop_result(&a);CHECK(luxrt_atomic_load64(&a)==24);atomic_drop_cas_result(&a);CHECK(luxrt_atomic_load64(&a)==25);CHECK(atomic_global_add()==0);CHECK(atomic_global_add()==1);
 uint64_t seed=0x3789456328;
 for(unsigned i=0;i<100000;i++){
  seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;uint64_t next=seed^0x839475924898;
  luxrt_atomic_store64(&a,seed);CHECK(atomic_register_stress(&a,1,2,3,4,5,6,7,8)==seed+1+3+36);CHECK(luxrt_atomic_load64(&a)==3);
  luxrt_atomic_store64(&a,seed);CHECK(luxrt_atomic_load64(&a)==seed);
  CHECK(luxrt_atomic_cas64(&a,seed,next)==seed);CHECK(luxrt_atomic_load64(&a)==next);
  CHECK(luxrt_atomic_cas64(&a,next+1,seed)==next);CHECK(luxrt_atomic_load64(&a)==next);
  luxrt_atomic_store32(&b,(uint32_t)seed);CHECK(luxrt_atomic_load32(&b)==(uint32_t)seed);
  CHECK(luxrt_atomic_cas32(&b,(uint32_t)seed,(uint32_t)next)==(uint32_t)seed);CHECK(luxrt_atomic_load32(&b)==(uint32_t)next);
  CHECK(luxrt_atomic_cas32(&b,(uint32_t)next+1,(uint32_t)seed)==(uint32_t)next);CHECK(luxrt_atomic_load32(&b)==(uint32_t)next);
 }
 luxrt_atomic_store64(&a,0);for(unsigned i=0;i<4;i++)CHECK(pthread_create(&threads[i],NULL,atomic_cas_worker,&a)==0);
 for(unsigned i=0;i<4;i++){CHECK(pthread_join(threads[i],NULL)==0);}
 CHECK(luxrt_atomic_load64(&a)==400000);
#endif
 printf("%u atomic checks passed (4-thread counters, 2000 SC ordering rounds)\n",checks);return 0;}
