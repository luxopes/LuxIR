#define _GNU_SOURCE
#include "../../runtime/os.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <termios.h>
#include <unistd.h>
static unsigned count;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s (errno=%d)\n",__LINE__,#c,errno);exit(1);}count++;}while(0)
static void descriptor(int64_t fd){CHECK(fd>=0);CHECK((fcntl((int)fd,F_GETFD)&FD_CLOEXEC)!=0);}
struct entry {uint64_t ino;int64_t off;unsigned short length;unsigned char type;char name[];};
static void files(void){
 char root[]="/tmp/luxrt-ext-XXXXXX";CHECK(mkdtemp(root)!=NULL);
 char a[PATH_MAX],b[PATH_MAX],link[PATH_MAX],dir[PATH_MAX],buffer[PATH_MAX];
 CHECK(snprintf(a,sizeof a,"%s/a",root)>0);CHECK(snprintf(b,sizeof b,"%s/b",root)>0);
 CHECK(snprintf(link,sizeof link,"%s/link",root)>0);CHECK(snprintf(dir,sizeof dir,"%s/dir",root)>0);
 CHECK(luxrt_os_mkdir(dir,0700)==0);CHECK(luxrt_os_mkdir(dir,0700)==-EEXIST);
 int64_t fd=luxrt_os_open(a,O_CREAT|O_EXCL|O_RDWR,0600);descriptor(fd);
 CHECK(luxrt_os_write(fd,"abc",3)==3);CHECK(luxrt_os_chmod(fd,0640)==0);
 CHECK(luxrt_os_close(fd)==0);struct statx st;memset(&st,0,sizeof st);
 CHECK(luxrt_os_stat(a,0,STATX_BASIC_STATS,&st)==0);CHECK(st.stx_size==3);
 struct stat reference;CHECK(stat(a,&reference)==0);CHECK(st.stx_mode==reference.st_mode);
 CHECK(luxrt_os_rename(a,b)==0);CHECK(luxrt_os_stat(a,0,STATX_BASIC_STATS,&st)==-ENOENT);
 CHECK(luxrt_os_symlink(b,link)==0);int64_t n=luxrt_os_readlink(link,buffer,sizeof buffer);
 CHECK(n==(int64_t)strlen(b));CHECK(memcmp(buffer,b,(size_t)n)==0);
 fd=luxrt_os_open(root,O_RDONLY|O_DIRECTORY,0);descriptor(fd);
 unsigned char entries[1024];n=luxrt_os_list(fd,entries,sizeof entries);CHECK(n>0);
 unsigned found=0;for(size_t pos=0;pos<(size_t)n;){struct entry *e=(void*)(entries+pos);
  CHECK(e->length>=20 && pos+e->length<=(size_t)n);CHECK(memchr(e->name,0,e->length-19)!=NULL);
  if(!strcmp(e->name,"b"))found|=1;
  if(!strcmp(e->name,"link"))found|=2;
  if(!strcmp(e->name,"dir"))found|=4;
  pos+=e->length;
 }CHECK(found==7);CHECK(luxrt_os_close(fd)==0);
 char saved[PATH_MAX],actual[PATH_MAX];CHECK(getcwd(saved,sizeof saved)!=NULL);
 CHECK(luxrt_os_cwd(buffer,sizeof buffer)==(int64_t)strlen(saved)+1);CHECK(!strcmp(buffer,saved));
 CHECK(luxrt_os_cwd(buffer,1)==-ERANGE);CHECK(luxrt_os_chdir(dir)==0);
 CHECK(getcwd(actual,sizeof actual)!=NULL);CHECK(!strcmp(actual,dir));CHECK(luxrt_os_chdir(saved)==0);
 CHECK(luxrt_os_unlink(link,0)==0);CHECK(luxrt_os_unlink(b,0)==0);CHECK(luxrt_os_unlink(dir,AT_REMOVEDIR)==0);
 CHECK(luxrt_os_unlink(root,AT_REMOVEDIR)==0);
}
static void processes(void){
 CHECK(luxrt_os_pid()==getpid());CHECK(luxrt_os_ppid()==getppid());
 CHECK(luxrt_os_exec("/luxrt/missing",NULL,NULL)==-ENOENT);
 int pipes[2];CHECK(luxrt_os_pipe(pipes,0)==0);descriptor(pipes[0]);descriptor(pipes[1]);
 int64_t child=luxrt_os_fork();CHECK(child>=0);
 if(child==0){
  if(luxrt_os_close(pipes[0]) || luxrt_os_write(pipes[1],"child",5)!=5)luxrt_os_exit(99);
  luxrt_os_close(pipes[1]);
  char *argv[]={"/bin/sh","-c","test \"$LUXRT_TEST\" = ok; exit $?",NULL};char *env[]={"LUXRT_TEST=ok",NULL};
  luxrt_os_exec(argv[0],argv,env);luxrt_os_exit(98);_exit(97);
 }
 CHECK(luxrt_os_close(pipes[1])==0);char data[8]={0};CHECK(luxrt_os_read(pipes[0],data,8)==5);CHECK(!memcmp(data,"child",5));
 CHECK(luxrt_os_close(pipes[0])==0);int status=0;CHECK(luxrt_os_wait(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 CHECK(luxrt_os_wait(child,&status,0)==-ECHILD);CHECK(luxrt_os_kill(getpid(),0)==0);
 int fd=open("/dev/null",O_RDONLY);CHECK(fd>=0);int high=fcntl(fd,F_DUPFD_CLOEXEC,100);CHECK(high>=100);
 CHECK(luxrt_os_dup(fd,high+1)==high+1);descriptor(high+1);
 /* close_range is allowed to report ENOSYS on older kernels. */
 int64_t rc=luxrt_os_close_range(high,high+1,0);CHECK(rc==0||rc==-ENOSYS);
 if(rc==0){CHECK(fcntl(high,F_GETFD)==-1&&errno==EBADF);CHECK(fcntl(high+1,F_GETFD)==-1&&errno==EBADF);}
 else {CHECK(close(high)==0);CHECK(close(high+1)==0);}CHECK(close(fd)==0);
}
static void sockets(void){
 int pair[2];CHECK(luxrt_os_socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);descriptor(pair[0]);descriptor(pair[1]);
 const char text[]="socket test";CHECK(luxrt_os_send(pair[0],(void*)text,sizeof text,0,NULL,0)==sizeof text);
 struct pollfd pollfd={pair[1],POLLIN,0};struct timespec zero={0,0};CHECK(luxrt_os_poll(&pollfd,1,&zero)==1);CHECK(pollfd.revents&POLLIN);
 char buf[64];CHECK(luxrt_os_recv(pair[1],buf,sizeof buf,0,NULL,NULL)==sizeof text);CHECK(!memcmp(buf,text,sizeof text));
 CHECK(luxrt_os_shutdown(pair[1],SHUT_RDWR)==0);CHECK(luxrt_os_send(pair[0],buf,1,0,NULL,0)==-EPIPE);
 CHECK(luxrt_os_close(pair[0])==0);CHECK(luxrt_os_close(pair[1])==0);
 int64_t server=luxrt_os_socket(AF_INET,SOCK_STREAM,0);descriptor(server);int yes=1;
 CHECK(luxrt_os_setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes)==0);
 int option=0;socklen_t len=sizeof option;CHECK(luxrt_os_getsockopt(server,SOL_SOCKET,SO_REUSEADDR,&option,&len)==0);CHECK(option==1);
 struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0};
 CHECK(luxrt_os_bind(server,&addr,sizeof addr)==0);len=sizeof addr;CHECK(luxrt_os_sockname(server,&addr,&len)==0);CHECK(addr.sin_port!=0);
 CHECK(luxrt_os_listen(server,1)==0);int64_t client=luxrt_os_socket(AF_INET,SOCK_STREAM,0);descriptor(client);
 CHECK(luxrt_os_connect(client,&addr,sizeof addr)==0);int64_t accepted=luxrt_os_accept(server,NULL,NULL,0);descriptor(accepted);
 struct sockaddr_in peer;len=sizeof peer;CHECK(luxrt_os_peername(client,&peer,&len)==0);CHECK(peer.sin_port==addr.sin_port);
 CHECK(luxrt_os_send(client,(void*)text,sizeof text,0,NULL,0)==sizeof text);CHECK(luxrt_os_recv(accepted,buf,sizeof buf,0,NULL,NULL)==sizeof text);CHECK(!memcmp(buf,text,sizeof text));
 CHECK(luxrt_os_close(accepted)==0);CHECK(luxrt_os_close(client)==0);CHECK(luxrt_os_close(server)==0);
 int64_t receiver=luxrt_os_socket(AF_INET,SOCK_DGRAM,0),sender=luxrt_os_socket(AF_INET,SOCK_DGRAM,0);descriptor(receiver);descriptor(sender);
 addr.sin_port=0;CHECK(luxrt_os_bind(receiver,&addr,sizeof addr)==0);len=sizeof addr;CHECK(luxrt_os_sockname(receiver,&addr,&len)==0);
 CHECK(luxrt_os_send(sender,(void*)text,sizeof text,0,&addr,sizeof addr)==sizeof text);len=sizeof peer;
 CHECK(luxrt_os_recv(receiver,buf,sizeof buf,0,&peer,&len)==sizeof text);CHECK(!memcmp(buf,text,sizeof text));
 CHECK(luxrt_os_close(receiver)==0);CHECK(luxrt_os_close(sender)==0);
}
static void terminal_wait(void){
 int fd=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);CHECK(fd>=0);CHECK(grantpt(fd)==0);CHECK(unlockpt(fd)==0);
 int64_t slave=luxrt_os_open(ptsname(fd),O_RDWR|O_NOCTTY,0);descriptor(slave);struct winsize set={24,80,0,0},got={0};
 CHECK(luxrt_os_ioctl(slave,TIOCSWINSZ,&set)==0);CHECK(luxrt_os_ioctl(slave,TIOCGWINSZ,&got)==0);CHECK(got.ws_row==24&&got.ws_col==80);
 /* Linux kernel termios has 19 control characters, unlike libc termios. */
 struct kernel_termios {unsigned int iflag,oflag,cflag,lflag;unsigned char line,cc[19];} term;
 CHECK(luxrt_os_ioctl(slave,TCGETS,&term)==0);term.lflag&=~(ICANON|ECHO);CHECK(luxrt_os_ioctl(slave,TCSETS,&term)==0);
 struct termios libc_term;CHECK(tcgetattr((int)slave,&libc_term)==0);CHECK(!(libc_term.c_lflag&(ICANON|ECHO)));
 CHECK(luxrt_os_close(slave)==0);CHECK(close(fd)==0);CHECK(luxrt_os_ioctl(-1,TCGETS,&term)==-EBADF);
 int word=1;CHECK(luxrt_os_futex(&word,128,0,NULL,NULL,0)==-EAGAIN);CHECK(luxrt_os_futex(&word,129,1,NULL,NULL,0)==0);
}
int main(void){files();processes();sockets();terminal_wait();printf("%u extended Linux OS checks passed\n",count);return 0;}
