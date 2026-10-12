#include "../thread.h"
#include "../atomic.h"
#include "../os.h"
#include <stdint.h>
extern int64_t luxrt_thread_clone(void (*fn)(void *),void *,void *,void *);
int64_t luxrt_thread_spawn(void (*fn)(void *),void *arg,void *stack,uint64_t size,void *tid){
 uintptr_t base=(uintptr_t)stack,slot=(uintptr_t)tid;
 if(!fn||!base||!slot||(slot&3)||size<4096||UINTPTR_MAX-base<size)return -22;
 uintptr_t end=(base+size)&~(uintptr_t)15;
 if(end-base<4096||luxrt_atomic_load32(tid))return -22;
 return luxrt_thread_clone(fn,arg,(void *)end,tid);
}
int64_t luxrt_thread_join(void *tid){
 if(!tid||((uintptr_t)tid&3))return -22;
 for(;;){
  uint32_t value=luxrt_atomic_load32(tid);if(!value)return 0;
  int64_t rc=luxrt_os_futex(tid,0,value,0,0,0);
  if(rc<0&&rc!=-4&&rc!=-11)return rc;
 }
}

extern int64_t luxrt_thread_clone_tls(void (*fn)(void *),void *,void *,void *,void *);
int64_t luxrt_thread_spawn_tls(void (*fn)(void *),void *arg,void *stack,uint64_t size,void *tid,void *tp){
 uintptr_t base=(uintptr_t)stack,slot=(uintptr_t)tid;
 if(!fn||!base||!slot||!tp||(slot&3)||size<4096||UINTPTR_MAX-base<size)return -22;
 uintptr_t end=(base+size)&~(uintptr_t)15;
 if(end-base<4096||luxrt_atomic_load32(tid))return -22;
 return luxrt_thread_clone_tls(fn,arg,(void *)end,tid,tp);
}
