#include <stdint.h>
#include "../runtime/atomic.h"
uint64_t atomic_barrier_check(void *p){uint64_t old=*(uint64_t*)p;luxrt_atomic_add64(p,1);return *(uint64_t*)p-old;}
uint64_t atomic_store_barrier_check(void *p){*(uint64_t*)p=17;luxrt_atomic_fence();uint64_t old=luxrt_atomic_swap64(p,23);return old+*(uint64_t*)p;}

static uint64_t global;
void atomic_drop_result(void *p){luxrt_atomic_add64(p,1);luxrt_atomic_load64(p);}
uint64_t atomic_global_add(void){return luxrt_atomic_add64(&global,1);}

void atomic_drop_cas_result(void *p){luxrt_atomic_cas64(p,24,25);}
void *atomic_cas_worker(void *p){for(unsigned i=0;i<100000;i++){uint64_t old;do{old=luxrt_atomic_load64(p);}while(luxrt_atomic_cas64(p,old,old+1)!=old);}return 0;}
uint64_t atomic_register_stress(void *p,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h){uint64_t old=luxrt_atomic_load64(p),v=luxrt_atomic_cas64(p,old,a),x=luxrt_atomic_add64(p,b),y=luxrt_atomic_swap64(p,c);return v+x+y+a+b+c+d+e+f+g+h;}
