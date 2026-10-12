#include "tls.h"
#include "os.h"
#include "memory.h"
#include "startup.h"
#include <stddef.h>
static uint64_t u64(const void *p){return *(const uint64_t*)p;}
static uint32_t u32(const void *p){return *(const uint32_t*)p;}
int64_t luxrt_tls_template_from_stack(const uint64_t *stack,struct luxrt_tls_template *out){
 char **env=luxrt_startup_env(stack);while(*env)env++;
 const uint64_t *aux=(const uint64_t*)(env+1);const unsigned char *ph=0;uint64_t count=0,ent=0;
 for(;aux[0];aux+=2){if(aux[0]==3)ph=(const void*)(uintptr_t)aux[1];if(aux[0]==4)ent=aux[1];if(aux[0]==5)count=aux[1];}
 if(!ph||!count||count>4096||ent!=56)return -22;
 uint64_t bias=0;int found=0;
 for(uint64_t i=0;i<count;i++){const unsigned char *p=ph+i*56;if(u32(p)==6){bias=(uintptr_t)ph-u64(p+16);found=1;break;}}
 if(!found){
  const unsigned char *eh=ph-64;
  if(u32(eh)!=0x464c457f||eh[4]!=2||u64(eh+32)!=64)return -95;
  for(uint64_t i=0;i<count;i++){const unsigned char *p=ph+i*56;if(u32(p)==1&&u64(p+8)==0){bias=(uintptr_t)eh-u64(p+16);found=1;break;}}
  if(!found)return -95;
 }
 const void *image=0;uint64_t filesz=0,memsz=0,align=16;
 for(uint64_t i=0;i<count;i++){
  const unsigned char *p=ph+i*56;if(u32(p)!=7)continue;
  uint64_t addr=u64(p+16);filesz=u64(p+32);memsz=u64(p+40);align=u64(p+48);if(!align)align=1;
  if(filesz>memsz||align>1048576||(align&(align-1))||(addr&(align-1)))return -22;
  image=(const void*)(uintptr_t)(bias+addr);break;
 }
 out->image=image;out->filesz=filesz;out->memsz=memsz;out->align=align;return 0;
}
int64_t luxrt_tls_allocate(const struct luxrt_tls_template *t,uint64_t model,struct luxrt_tls_block *out){
 uint64_t align=t->align,template_align=align;
 if(!align||(align&(align-1)))return -22;
 if(align<16)align=16;
 if(model<1||model>3||t->filesz>t->memsz||(t->filesz&&!t->image)||align>1048576||(align&(align-1)))return -22;
 if(t->memsz>INT64_MAX-align*3-64)return -34;
 uint64_t span=(t->memsz+template_align-1)&~(template_align-1),offset=model==2?((16+align-1)&~(align-1)):0;
 uint64_t size=span+align*2+64;int64_t raw=luxrt_os_map(size);if(raw<0)return raw;
 uintptr_t base=(uintptr_t)raw,tp;void *data;
 if(model==1){tp=(base+span+align-1)&~(uintptr_t)(align-1);data=(void*)(tp-span);*(uintptr_t*)tp=tp;*(uintptr_t*)(tp+16)=tp;}
 else {tp=(base+align-1)&~(uintptr_t)(align-1);data=(void*)(tp+offset);}
 if(t->filesz)luxrt_memcpy(data,t->image,t->filesz);
 out->mapping=(void*)base;out->size=size;out->tp=(void*)tp;return 0;
}
int64_t luxrt_tls_allocate_native(const struct luxrt_tls_template *t,struct luxrt_tls_block *out){
#if defined(__x86_64__)
 return luxrt_tls_allocate(t,1,out);
#elif defined(__aarch64__)
 return luxrt_tls_allocate(t,2,out);
#elif defined(__riscv) && __riscv_xlen==64
 return luxrt_tls_allocate(t,3,out);
#else
#error unsupported static TLS architecture
#endif
}
int64_t luxrt_tls_release(struct luxrt_tls_block *block){
 if(block->tp==luxrt_tls_get())return -16;
 int64_t result=luxrt_os_unmap(block->mapping,block->size);if(!result){block->mapping=0;block->size=0;block->tp=0;}return result;
}
int64_t luxrt_tls_main(const uint64_t *stack,struct luxrt_tls_block *out){
 struct luxrt_tls_template t;int64_t rc=luxrt_tls_template_from_stack(stack,&t);if(rc)return rc;
 rc=luxrt_tls_allocate_native(&t,out);if(rc)return rc;
 rc=luxrt_tls_set(out->tp);if(rc)luxrt_tls_release(out);return rc;
}
