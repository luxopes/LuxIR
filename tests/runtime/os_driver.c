#define _GNU_SOURCE
#include "../../runtime/os.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);exit(1);}count++;}while(0)
int main(void){unsigned count=0;size_t size=(size_t)sysconf(_SC_PAGESIZE);int64_t p=luxrt_os_map(size);CHECK(p>0);unsigned char *data=(void*)(uintptr_t)p;
 data[0]=17;data[size-1]=42;CHECK(data[0]==17&&data[size-1]==42);
 CHECK(luxrt_os_protect(data,size,PROT_READ)==0);CHECK(data[size-1]==42);
 CHECK(luxrt_os_protect(data,size,PROT_READ|PROT_WRITE)==0);CHECK(luxrt_os_unmap(data,size)==0);
 CHECK(luxrt_os_map(0)==-EINVAL);CHECK(luxrt_os_close(-1)==-EBADF);
 CHECK(luxrt_os_read(-1,data,0)==-EBADF);CHECK(luxrt_os_write(-1,data,0)==-EBADF);
 char path[]="/tmp/luxrt-os-XXXXXX";int initial=mkstemp(path);CHECK(initial>=0);CHECK(close(initial)==0);
 int64_t fd=luxrt_os_open(path,O_RDWR|O_TRUNC,0600);CHECK(fd>=0);CHECK((fcntl((int)fd,F_GETFD)&FD_CLOEXEC)!=0);
 const char text[]="luxrt syscall test";CHECK(luxrt_os_write(fd,text,sizeof text)==sizeof text);CHECK(luxrt_os_seek(fd,0,SEEK_SET)==0);
 char buffer[sizeof text];CHECK(luxrt_os_read(fd,buffer,sizeof buffer)==sizeof buffer);CHECK(memcmp(text,buffer,sizeof text)==0);
 CHECK(luxrt_os_close(fd)==0);CHECK(unlink(path)==0);CHECK(luxrt_os_open(path,O_RDONLY,0)==-ENOENT);
 unsigned char random[32];CHECK(luxrt_os_random(random,sizeof random,0)==sizeof random);
 struct timespec before,after;CHECK(luxrt_os_clock(CLOCK_MONOTONIC,&before)==0);struct timespec delay={0,1000000};CHECK(luxrt_os_sleep(&delay,NULL)==0);CHECK(luxrt_os_clock(CLOCK_MONOTONIC,&after)==0);
 CHECK(after.tv_sec>before.tv_sec||(after.tv_sec==before.tv_sec&&after.tv_nsec>=before.tv_nsec));
 struct timespec invalid={0,1000000000};CHECK(luxrt_os_sleep(&invalid,NULL)==-EINVAL);
 printf("%u Linux syscall checks passed\n",count);return 0;}
