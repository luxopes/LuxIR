#define _GNU_SOURCE
#include "../../runtime/tls.h"
#include "../../runtime/thread.h"
#include "../../runtime/atomic.h"
#include "../../runtime/os.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef LUXRT_IR_WORKER
extern __thread volatile uint64_t private_value,initial;
extern __thread volatile unsigned char pad[103];
extern void worker(void *);
#else
static __thread volatile uint64_t private_value=7;
static __thread volatile uint64_t initial=1234567;
static __thread volatile unsigned char pad[103] __attribute__((aligned(64)));

#endif
struct result {uint64_t id;_Atomic uint32_t ok;uint64_t final;};
#ifndef LUXRT_IR_WORKER
static void worker(void *arg){
 struct result *r=arg;uint32_t ok=private_value==7&&initial==1234567;
 for(unsigned i=0;i<sizeof pad;i++)if(pad[i])ok=0;
 private_value=r->id;initial=r->id+1000;
 for(unsigned i=0;i<10000;i++){private_value++;pad[i%sizeof pad]=(unsigned char)r->id;}
 if(private_value!=r->id+10000||initial!=r->id+1000)ok=0;
 r->final=private_value;luxrt_atomic_store32(&r->ok,ok);
}
#endif
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;uint64_t stack[]={0,0,0,3,getauxval(AT_PHDR),4,getauxval(AT_PHENT),5,getauxval(AT_PHNUM),0,0};
 struct luxrt_tls_template t;CHECK(luxrt_tls_template_from_stack(stack,&t)==0);CHECK(t.filesz<=t.memsz&&t.align>=64);
 struct luxrt_tls_template bad=t;struct luxrt_tls_block untouched={(void*)1,2,(void*)3},test=untouched;
 bad.align=3;CHECK(luxrt_tls_allocate(&bad,1,&test)==-22);CHECK(test.mapping==(void*)1&&test.size==2&&test.tp==(void*)3);
 bad=t;bad.image=NULL;CHECK(luxrt_tls_allocate(&bad,1,&test)==-22);
 bad=t;bad.memsz=INT64_MAX-t.align*2-64;CHECK(luxrt_tls_allocate(&bad,1,&test)==-34);
 bad=t;bad.memsz=UINT64_MAX;CHECK(luxrt_tls_allocate(&bad,1,&test)==-34);CHECK(luxrt_tls_allocate(&t,0,&test)==-22);
 stack[6]=1;CHECK(luxrt_tls_template_from_stack(stack,&bad)==-22);stack[6]=56;
 size_t page=(size_t)sysconf(_SC_PAGESIZE),size=16*page;void *maps[40];struct luxrt_tls_block blocks[40];_Atomic uint32_t tids[40]={0};struct result results[40]={0};
 for(unsigned i=0;i<40;i++){
  results[i].id=i+1;CHECK(luxrt_tls_allocate_native(&t,&blocks[i])==0);
  maps[i]=mmap(NULL,size+page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(maps[i]!=MAP_FAILED);CHECK(mprotect(maps[i],page,PROT_NONE)==0);
  CHECK(luxrt_thread_spawn_tls(worker,&results[i],(char*)maps[i]+page,size,&tids[i],blocks[i].tp)>0);
 }
 for(unsigned i=0;i<40;i++){
  CHECK(luxrt_thread_join(&tids[i])==0);CHECK(luxrt_atomic_load32(&results[i].ok)==1);CHECK(results[i].final==i+10001);
  CHECK(luxrt_tls_release(&blocks[i])==0);CHECK(blocks[i].mapping==NULL&&blocks[i].size==0&&blocks[i].tp==NULL);
  CHECK(munmap(maps[i],size+page)==0);
 }
 CHECK(private_value==7&&initial==1234567);for(unsigned i=0;i<sizeof pad;i++)CHECK(pad[i]==0);
 printf("%u static TLS checks passed (40 private threads, initialized + zero-fill + aligned TLS)\n",checks);return 0;
}
