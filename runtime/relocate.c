#include "relocate.h"
#include "startup.h"
#include "os.h"
#include <stdint.h>
int64_t luxrt_startup_relocate(const uint64_t *stack,void *base,const uint64_t *dynamic,uint64_t relative_type){
 uint64_t addr=0,size=0,ent=0,offset=(uintptr_t)base;
 for(uint64_t n=0;dynamic[0];n++,dynamic+=2){
  if(n>=4096)return -22;
  if(dynamic[0]==7)addr=dynamic[1];
  if(dynamic[0]==8)size=dynamic[1];
  if(dynamic[0]==9)ent=dynamic[1];
  if((dynamic[0]==17||dynamic[0]==18)&&dynamic[1])return -95;
 }
 if(size){
  if(!addr||ent!=24||size%24)return -22;
  uint64_t *rela=(void*)(uintptr_t)(offset+addr);
  for(uint64_t n=0;n<size;n+=24,rela+=3){
   if(!rela[1])continue;
   if(rela[1]!=relative_type)return -95;
   *(uint64_t*)(uintptr_t)(offset+rela[0])=offset+rela[2];
  }
 }
 char **env=luxrt_startup_env(stack);while(*env)env++;
 uint64_t *aux=(void*)(env+1);uint64_t *ph=0,phent=0,count=0,page=luxrt_startup_pagesize(stack);
 for(;aux[0];aux+=2){if(aux[0]==3)ph=(void*)(uintptr_t)aux[1];if(aux[0]==4)phent=aux[1];if(aux[0]==5)count=aux[1];}
 if(!ph||phent!=56||!count||count>4096||!page||(page&(page-1)))return -22;
 for(uint64_t n=0;n<count;n++,ph+=7){
  if((uint32_t)ph[0]!=0x6474e552)continue;
  uint64_t start=(offset+ph[2])&~(page-1),end=(offset+ph[2]+ph[5])&~(page-1);
  if(end>start){int64_t rc=luxrt_os_protect((void*)(uintptr_t)start,end-start,1);if(rc)return rc;}
 }
 return 0;
}
