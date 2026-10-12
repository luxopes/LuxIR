#include "../../runtime/startup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static unsigned checks;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d\n",__LINE__);exit(1);}checks++;}while(0)
int main(void){
 char *env[]={"ABC=one","ABCD=two","AB=","=invalid","X=one=two",NULL};
 CHECK(!strcmp(luxrt_getenv(env,"ABC"),"one"));CHECK(!strcmp(luxrt_getenv(env,"ABCD"),"two"));
 CHECK(!strcmp(luxrt_getenv(env,"AB"),""));CHECK(!strcmp(luxrt_getenv(env,"X"),"one=two"));
 CHECK(luxrt_getenv(env,"A")==NULL);CHECK(luxrt_getenv(env,"ABCDE")==NULL);
 CHECK(luxrt_getenv(env,"")==NULL);CHECK(luxrt_getenv(env,"ABC=")==NULL);
 char *empty[]={NULL};CHECK(luxrt_getenv(empty,"X")==NULL);
 for(unsigned argc=0;argc<100;argc++)for(unsigned envc=0;envc<20;envc++){
  uint64_t stack[160]={0};stack[0]=argc;
  for(unsigned i=0;i<argc;i++)stack[i+1]=(uintptr_t)"argument";
  for(unsigned i=0;i<envc;i++)stack[argc+2+i]=(uintptr_t)"VAR=value";
  unsigned aux=argc+3+envc;stack[aux]=25;stack[aux+1]=1234;stack[aux+2]=6;stack[aux+3]=16384;
  CHECK(luxrt_startup_argc(stack)==argc);CHECK(luxrt_startup_argv(stack)==(char **)(stack+1));
  CHECK(luxrt_startup_env(stack)==(char **)(stack+argc+2));CHECK(luxrt_startup_pagesize(stack)==16384);
  stack[aux+2]=0;CHECK(luxrt_startup_pagesize(stack)==0);
 }
 printf("%u startup/environment checks passed\n",checks);return 0;
}
