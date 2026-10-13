#include "../os.h"
#include <stdint.h>
extern int64_t luxrt_syscall6(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);
#define P(x) ((int64_t)(uintptr_t)(x))
int64_t luxrt_os_map(uint64_t n){return luxrt_syscall6(9,0,n,3,34,-1,0);}
int64_t luxrt_os_unmap(void *p,uint64_t n){return luxrt_syscall6(11,P(p),n,0,0,0,0);}
int64_t luxrt_os_protect(void *p,uint64_t n,int64_t prot){return luxrt_syscall6(10,P(p),n,prot,0,0,0);}
/* openat with AT_FDCWD; descriptors are always close-on-exec. */
int64_t luxrt_os_open(const char *p,int64_t flags,int64_t mode){return luxrt_syscall6(257,-100,P(p),flags|02000000,mode,0,0);}
int64_t luxrt_os_read(int64_t f,void *p,uint64_t n){return luxrt_syscall6(0,f,P(p),n,0,0,0);}
int64_t luxrt_os_write(int64_t f,const void *p,uint64_t n){return luxrt_syscall6(1,f,P(p),n,0,0,0);}
int64_t luxrt_os_close(int64_t f){return luxrt_syscall6(3,f,0,0,0,0,0);}
int64_t luxrt_os_seek(int64_t f,int64_t offset,int64_t whence){return luxrt_syscall6(8,f,offset,whence,0,0,0);}
int64_t luxrt_os_random(void *p,uint64_t n,int64_t flags){return luxrt_syscall6(318,P(p),n,flags,0,0,0);}
int64_t luxrt_os_clock(int64_t id,void *p){return luxrt_syscall6(228,id,P(p),0,0,0,0);}
int64_t luxrt_os_sleep(const void *p,void *remaining){return luxrt_syscall6(35,P(p),P(remaining),0,0,0,0);}

int64_t luxrt_os_stat(const char * path,int64_t flags,uint64_t mask,void * result){return luxrt_syscall6(332,-100,P(path),flags,mask,P(result),0);}

int64_t luxrt_os_list(int64_t fd,void * buffer,uint64_t size){return luxrt_syscall6(217,fd,P(buffer),size,0,0,0);}

int64_t luxrt_os_mkdir(const char * path,uint64_t mode){return luxrt_syscall6(258,-100,P(path),mode,0,0,0);}

int64_t luxrt_os_unlink(const char * path,int64_t flags){return luxrt_syscall6(263,-100,P(path),flags,0,0,0);}

int64_t luxrt_os_rename(const char * oldpath,const char * newpath){return luxrt_syscall6(264,-100,P(oldpath),-100,P(newpath),0,0);}

int64_t luxrt_os_chmod(int64_t fd,uint64_t mode){return luxrt_syscall6(91,fd,mode,0,0,0,0);}

int64_t luxrt_os_symlink(const char * target,const char * path){return luxrt_syscall6(266,P(target),-100,P(path),0,0,0);}

int64_t luxrt_os_readlink(const char * path,void * buffer,uint64_t size){return luxrt_syscall6(267,-100,P(path),P(buffer),size,0,0);}

int64_t luxrt_os_cwd(void * buffer,uint64_t size){return luxrt_syscall6(79,P(buffer),size,0,0,0,0);}

int64_t luxrt_os_chdir(const char * path){return luxrt_syscall6(80,P(path),0,0,0,0,0);}

int64_t luxrt_os_pipe(void * fds,int64_t flags){int64_t cloexec=flags|524288;return luxrt_syscall6(293,P(fds),cloexec,0,0,0,0);}

int64_t luxrt_os_dup(int64_t fd,int64_t newfd){return luxrt_syscall6(292,fd,newfd,524288,0,0,0);}

int64_t luxrt_os_wait(int64_t pid,void * status,int64_t options){return luxrt_syscall6(61,pid,P(status),options,0,0,0);}

int64_t luxrt_os_kill(int64_t pid,int64_t signal){return luxrt_syscall6(62,pid,signal,0,0,0,0);}

int64_t luxrt_os_exec(const char * path,void * argv,void * envp){return luxrt_syscall6(59,P(path),P(argv),P(envp),0,0,0);}

int64_t luxrt_os_pid(void){return luxrt_syscall6(39,0,0,0,0,0,0);}

int64_t luxrt_os_ppid(void){return luxrt_syscall6(110,0,0,0,0,0,0);}

int64_t luxrt_os_close_range(uint64_t first,uint64_t last,uint64_t flags){return luxrt_syscall6(436,first,last,flags,0,0,0);}

int64_t luxrt_os_exit(int64_t status){return luxrt_syscall6(231,status,0,0,0,0,0);}

int64_t luxrt_os_socket(int64_t domain,int64_t type,int64_t protocol){int64_t cloexec=type|524288;return luxrt_syscall6(41,domain,cloexec,protocol,0,0,0);}

int64_t luxrt_os_socketpair(int64_t domain,int64_t type,int64_t protocol,void * fds){int64_t cloexec=type|524288;return luxrt_syscall6(53,domain,cloexec,protocol,P(fds),0,0);}

int64_t luxrt_os_bind(int64_t fd,void * address,uint64_t length){return luxrt_syscall6(49,fd,P(address),length,0,0,0);}

int64_t luxrt_os_listen(int64_t fd,int64_t backlog){return luxrt_syscall6(50,fd,backlog,0,0,0,0);}

int64_t luxrt_os_accept(int64_t fd,void * address,void * length,int64_t flags){int64_t cloexec=flags|524288;return luxrt_syscall6(288,fd,P(address),P(length),cloexec,0,0);}

int64_t luxrt_os_connect(int64_t fd,void * address,uint64_t length){return luxrt_syscall6(42,fd,P(address),length,0,0,0);}

int64_t luxrt_os_send(int64_t fd,void * buffer,uint64_t size,int64_t flags,void * address,uint64_t length){int64_t nosignal=flags|16384;return luxrt_syscall6(44,fd,P(buffer),size,nosignal,P(address),length);}

int64_t luxrt_os_recv(int64_t fd,void * buffer,uint64_t size,int64_t flags,void * address,void * length){return luxrt_syscall6(45,fd,P(buffer),size,flags,P(address),P(length));}

int64_t luxrt_os_shutdown(int64_t fd,int64_t how){return luxrt_syscall6(48,fd,how,0,0,0,0);}

int64_t luxrt_os_sockname(int64_t fd,void * address,void * length){return luxrt_syscall6(51,fd,P(address),P(length),0,0,0);}

int64_t luxrt_os_peername(int64_t fd,void * address,void * length){return luxrt_syscall6(52,fd,P(address),P(length),0,0,0);}

int64_t luxrt_os_setsockopt(int64_t fd,int64_t level,int64_t option,void * value,uint64_t length){return luxrt_syscall6(54,fd,level,option,P(value),length,0);}

int64_t luxrt_os_getsockopt(int64_t fd,int64_t level,int64_t option,void * value,void * length){return luxrt_syscall6(55,fd,level,option,P(value),P(length),0);}

int64_t luxrt_os_poll(void * fds,uint64_t count,void * timeout){return luxrt_syscall6(271,P(fds),count,P(timeout),0,0,0);}

int64_t luxrt_os_ioctl(int64_t fd,uint64_t request,void * argument){return luxrt_syscall6(16,fd,request,P(argument),0,0,0);}

int64_t luxrt_os_futex(void * address,int64_t operation,int64_t value,void * timeout,void * address2,int64_t value3){return luxrt_syscall6(202,P(address),operation,value,P(timeout),P(address2),value3);}

int64_t luxrt_os_fork(void){return luxrt_syscall6(57,0,0,0,0,0,0);}

int64_t luxrt_os_dup_flags(int64_t fd,int64_t newfd,int64_t flags){return luxrt_syscall6(292,fd,newfd,flags,0,0,0);}
int64_t luxrt_os_fcntl(int64_t fd,int64_t command,int64_t argument){return luxrt_syscall6(72,fd,command,argument,0,0,0);}
int64_t luxrt_os_fstatfs(int64_t fd,void *buffer){return luxrt_syscall6(138,fd,P(buffer),0,0,0,0);}

int64_t luxrt_os_setpgid(int64_t pid,int64_t group){return luxrt_syscall6(109,pid,group,0,0,0,0);}
