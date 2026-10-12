#include "../signal.h"
#include <stdint.h>
extern int64_t luxrt_syscall6(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);
extern void luxrt_sigreturn(void);
#define P(x) ((int64_t)(uintptr_t)(x))
struct native_action {void (*handler)(int);uint64_t flags;void (*restorer)(void);uint64_t mask;};
int64_t luxrt_signal_action(int64_t sig,const struct luxrt_signal_action *action,struct luxrt_signal_action *old){
 struct native_action in,out;
 if(action){in.handler=action->handler;in.flags=action->flags|0x04000000;in.restorer=luxrt_sigreturn;in.mask=action->mask;}
 int64_t rc=luxrt_syscall6(13,sig,action?P(&in):0,old?P(&out):0,8,0,0);
 if(!rc&&old){old->handler=out.handler;old->flags=out.flags&~(uint64_t)0x04000000;old->mask=out.mask;}
 return rc;
}
int64_t luxrt_signal_mask(int64_t how,const uint64_t *mask,uint64_t *old){return luxrt_syscall6(14,how,P(mask),P(old),8,0,0);}
int64_t luxrt_signal_thread(int64_t pid,int64_t tid,int64_t sig){return luxrt_syscall6(234,pid,tid,sig,0,0,0);}
