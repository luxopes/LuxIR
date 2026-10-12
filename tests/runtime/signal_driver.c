#define _GNU_SOURCE
#include "../../runtime/signal.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#ifdef LUXRT_IR_HANDLER
extern volatile sig_atomic_t deliveries,last_signal;
extern void handler(int);
#else
volatile sig_atomic_t deliveries,last_signal;
static void handler(int sig){last_signal=sig;deliveries++;}
#endif
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;
 struct luxrt_signal_action action={handler,0,0},old,query,unchanged={(void(*)(int))(uintptr_t)123,456,789},failed=unchanged;
 CHECK(luxrt_signal_action(0,&action,&failed)==-22);CHECK(memcmp(&failed,&unchanged,sizeof failed)==0);
 CHECK(luxrt_signal_action(SIGUSR1,&action,&old)==0);
 CHECK(luxrt_signal_action(SIGUSR1,NULL,&query)==0);CHECK(query.handler==handler&&query.flags==0&&query.mask==0);
 uint64_t mask=UINT64_C(1)<<(SIGUSR1-1),saved,queried=0;
 CHECK(luxrt_signal_mask(SIG_BLOCK,&mask,&saved)==0);CHECK(luxrt_signal_mask(SIG_BLOCK,NULL,&queried)==0);CHECK(queried&mask);
 CHECK(luxrt_signal_thread(getpid(),syscall(SYS_gettid),SIGUSR1)==0);CHECK(deliveries==0);
 CHECK(luxrt_signal_mask(SIG_UNBLOCK,&mask,NULL)==0);CHECK(deliveries==1&&last_signal==SIGUSR1);
 CHECK(luxrt_signal_thread(getpid(),syscall(SYS_gettid),SIGUSR1)==0);CHECK(deliveries==2&&last_signal==SIGUSR1);
 CHECK(luxrt_signal_mask(SIG_SETMASK,&saved,NULL)==0);CHECK(luxrt_signal_action(SIGUSR1,&old,NULL)==0);
 printf("%u signal checks passed (blocked delivery, native handler return, restored action/mask)\n",checks);return 0;
}
