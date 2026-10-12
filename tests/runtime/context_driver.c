#include "../../runtime/context.h"
#include "../../runtime/os.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern void *context_fixture(uint64_t *,uint64_t *);
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;uint64_t snapshot[27],token=0;
 for(unsigned i=0;i<27;i++)snapshot[i]=UINT64_C(0x1234abcdef);
 uint64_t words=luxrt_context_words();CHECK(words==7||words==20||words==25);
 void *sp=context_fixture(snapshot+1,&token);CHECK((uintptr_t)sp>4096);
 CHECK(snapshot[0]==UINT64_C(0x1234abcdef)&&snapshot[words+1]==UINT64_C(0x1234abcdef));
 CHECK(snapshot[words]==(uintptr_t)sp);
 /* Toy conservative mark: allocation metadata is encoded, so it is not a root.
    The fixture clears its allocation result from caller-saved registers. */
 unsigned marked=0;for(unsigned i=1;i<words;i++)if((snapshot[i]^UINT64_C(0x5a5a5a5a5a5a5a5a))==token)marked++;
 CHECK(marked==1);void *object=(void*)(uintptr_t)(token^UINT64_C(0x5a5a5a5a5a5a5a5a));
 CHECK(*(uint64_t*)object==42);CHECK(luxrt_os_unmap(object,4096)==0);
 uint64_t startup[4]={0};CHECK(luxrt_startup_stack_top(startup)==startup);
 printf("%u context checks passed (register-only fixture root, caller SP, snapshot bounds)\n",checks);return 0;
}
