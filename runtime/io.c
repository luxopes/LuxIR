#include "io.h"
#include "os.h"
int64_t luxrt_read_full(int64_t fd,void *buffer,uint64_t size,uint64_t *count){
 *count=0;if(size>INT64_MAX)return -34;
 while(*count<size){
  int64_t n=luxrt_os_read(fd,(char*)buffer+*count,size-*count);
  if(n==-4)continue;
  if(n<0)return n;
  if(!n)return 0;
  *count+=(uint64_t)n;
 }
 return 0;
}
int64_t luxrt_write_full(int64_t fd,const void *buffer,uint64_t size,uint64_t *count){
 *count=0;if(size>INT64_MAX)return -34;
 while(*count<size){
  int64_t n=luxrt_os_write(fd,(const char*)buffer+*count,size-*count);
  if(n==-4)continue;
  if(n<0)return n;
  if(!n)return -5;
  *count+=(uint64_t)n;
 }
 return 0;
}
