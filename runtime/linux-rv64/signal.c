#include "../signal.h"
#include <stdint.h>
extern int64_t luxrt_syscall6(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);
extern void luxrt_sigreturn(void);
#define P(x) ((int64_t)(uintptr_t)(x))
int64_t luxrt_signal_action(int64_t sig,const struct luxrt_signal_action *a,struct luxrt_signal_action *old){return luxrt_syscall6(134,sig,P(a),P(old),8,0,0);}
int64_t luxrt_signal_mask(int64_t how,const uint64_t *mask,uint64_t *old){return luxrt_syscall6(135,how,P(mask),P(old),8,0,0);}
int64_t luxrt_signal_thread(int64_t pid,int64_t tid,int64_t sig){return luxrt_syscall6(131,pid,tid,sig,0,0,0);}
