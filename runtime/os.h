#ifndef LUXRT_OS_H
#define LUXRT_OS_H
#include <stdint.h>
/* Linux raw results: nonnegative success, negative errno; no ambient libc errno. */
int64_t luxrt_os_map(uint64_t size);
int64_t luxrt_os_unmap(void *address,uint64_t size);
int64_t luxrt_os_protect(void *address,uint64_t size,int64_t protection);
int64_t luxrt_os_open(const char *path,int64_t flags,int64_t mode);
int64_t luxrt_os_read(int64_t fd,void *buffer,uint64_t size);
int64_t luxrt_os_write(int64_t fd,const void *buffer,uint64_t size);
int64_t luxrt_os_close(int64_t fd);
int64_t luxrt_os_seek(int64_t fd,int64_t offset,int64_t whence);
int64_t luxrt_os_random(void *buffer,uint64_t size,int64_t flags);
int64_t luxrt_os_clock(int64_t clock_id,void *timespec);
int64_t luxrt_os_sleep(const void *timespec,void *remaining);


/* Extended raw Linux API; stat uses Linux struct statx, list linux_dirent64.
 * NUL paths, kernel-layout buffers and valid spans are caller obligations.
 * pipe/socket/accept/dup are CLOEXEC; send also sets MSG_NOSIGNAL. */
int64_t luxrt_os_stat(const char * path,int64_t flags,uint64_t mask,void * result);
int64_t luxrt_os_list(int64_t fd,void * buffer,uint64_t size);
int64_t luxrt_os_mkdir(const char * path,uint64_t mode);
int64_t luxrt_os_unlink(const char * path,int64_t flags);
int64_t luxrt_os_rename(const char * oldpath,const char * newpath);
int64_t luxrt_os_chmod(int64_t fd,uint64_t mode);
int64_t luxrt_os_symlink(const char * target,const char * path);
int64_t luxrt_os_readlink(const char * path,void * buffer,uint64_t size);
int64_t luxrt_os_cwd(void * buffer,uint64_t size);
int64_t luxrt_os_chdir(const char * path);
int64_t luxrt_os_pipe(void * fds,int64_t flags);
int64_t luxrt_os_dup(int64_t fd,int64_t newfd);
int64_t luxrt_os_wait(int64_t pid,void * status,int64_t options);
int64_t luxrt_os_kill(int64_t pid,int64_t signal);
int64_t luxrt_os_exec(const char * path,void * argv,void * envp);
int64_t luxrt_os_pid(void);
int64_t luxrt_os_ppid(void);
int64_t luxrt_os_close_range(uint64_t first,uint64_t last,uint64_t flags);
int64_t luxrt_os_exit(int64_t status);
int64_t luxrt_os_socket(int64_t domain,int64_t type,int64_t protocol);
int64_t luxrt_os_socketpair(int64_t domain,int64_t type,int64_t protocol,void * fds);
int64_t luxrt_os_bind(int64_t fd,void * address,uint64_t length);
int64_t luxrt_os_listen(int64_t fd,int64_t backlog);
int64_t luxrt_os_accept(int64_t fd,void * address,void * length,int64_t flags);
int64_t luxrt_os_connect(int64_t fd,void * address,uint64_t length);
int64_t luxrt_os_send(int64_t fd,void * buffer,uint64_t size,int64_t flags,void * address,uint64_t length);
int64_t luxrt_os_recv(int64_t fd,void * buffer,uint64_t size,int64_t flags,void * address,void * length);
int64_t luxrt_os_shutdown(int64_t fd,int64_t how);
int64_t luxrt_os_sockname(int64_t fd,void * address,void * length);
int64_t luxrt_os_peername(int64_t fd,void * address,void * length);
int64_t luxrt_os_setsockopt(int64_t fd,int64_t level,int64_t option,void * value,uint64_t length);
int64_t luxrt_os_getsockopt(int64_t fd,int64_t level,int64_t option,void * value,void * length);
int64_t luxrt_os_poll(void * fds,uint64_t count,void * timeout);
int64_t luxrt_os_ioctl(int64_t fd,uint64_t request,void * argument);
int64_t luxrt_os_futex(void * address,int64_t operation,int64_t value,void * timeout,void * address2,int64_t value3);
int64_t luxrt_os_fork(void); /* x86_64 only; single-threaded caller */
#endif
