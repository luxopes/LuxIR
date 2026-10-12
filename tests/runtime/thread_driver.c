#define _GNU_SOURCE
#include "../../runtime/thread.h"
#include "../../runtime/atomic.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
static _Atomic uint64_t counter;
static void worker(void *arg){uint64_t n=(uintptr_t)arg;for(uint64_t i=0;i<n;i++)luxrt_atomic_add64(&counter,1);}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d\n",__LINE__);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;size_t page=(size_t)sysconf(_SC_PAGESIZE),size=page*16;_Atomic uint32_t tid[4]={0};void *mapping[4];
 CHECK(luxrt_thread_join(NULL)==-22);CHECK(luxrt_thread_join((void*)1)==-22);CHECK(luxrt_thread_join(&tid[0])==0);
 CHECK(luxrt_thread_spawn(NULL,NULL,(void*)16,4096,&tid[0])==-22);
 CHECK(luxrt_thread_spawn(worker,NULL,(void*)16,UINT64_MAX,&tid[0])==-22);
 CHECK(luxrt_thread_spawn(worker,NULL,(void*)16,8,&tid[0])==-22);
 for(unsigned repeat=0;repeat<10;repeat++){
  luxrt_atomic_store64(&counter,0);
  for(unsigned i=0;i<4;i++){
   mapping[i]=mmap(NULL,size+page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(mapping[i]!=MAP_FAILED);
   CHECK(mprotect(mapping[i],page,PROT_NONE)==0);
   CHECK(luxrt_thread_spawn(worker,(void*)100000,(char*)mapping[i]+page,size,&tid[i])>0);
  }
  for(unsigned i=0;i<4;i++){CHECK(luxrt_thread_join(&tid[i])==0);CHECK(luxrt_atomic_load32(&tid[i])==0);CHECK(munmap(mapping[i],size+page)==0);}
  CHECK(luxrt_atomic_load64(&counter)==400000);
 }
 printf("%u raw thread checks passed (40 threads, 4000000 atomic increments)\n",checks);return 0;}
