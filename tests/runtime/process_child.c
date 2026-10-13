#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int write_all(int fd,const char *data,size_t size) {
 while(size){ssize_t n=write(fd,data,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;data+=n;size-=(size_t)n;}return 0;
}
int main(int argc,char **argv) {
    for (int fd=3;fd<4096;fd++) if(fcntl(fd,F_GETFD)>=0) return 81;
    if (argc<2) return 80;
    if (!strcmp(argv[1],"exit")) return 7;
    if (!strcmp(argv[1],"echo")) {
        char data[4];int n=0;
        while(n<4){ssize_t got=read(0,data+n,4-n);if(got<0&&errno==EINTR)continue;if(got<=0)return 82;n+=(int)got;}
        const char *env=getenv("LUXRT_PROCESS_KEY");
        if(!env||strcmp(env,"value with spaces")||getenv("PATH"))return 83;
        if(write(1,data,4)!=4||write(2,"ERR!",4)!=4)return 84;
        return 23;
    }
    if (!strcmp(argv[1],"group")) return getpgrp()==getpid()?0:91;
    if (!strcmp(argv[1],"mask")) {sigset_t mask;sigprocmask(SIG_SETMASK,0,&mask);return sigismember(&mask,SIGCHLD)==atoi(argv[2])?0:92;}
    if (!strcmp(argv[1],"flood")) {
        char data[8192];memset(data,'P',sizeof data);
        for(int i=0;i<128;i++)if(write_all(1,data,sizeof data))return 87;
        memset(data,'E',sizeof data);
        for(int i=0;i<128;i++)if(write_all(2,data,sizeof data))return 88;
        for(;;){ssize_t n=read(0,data,sizeof data);if(n<0&&errno==EINTR)continue;if(n<0)return 89;if(!n)break;if(write_all(1,data,(size_t)n))return 90;}
        return 29;
    }
    if (!strcmp(argv[1],"closed-pause")) {close(0);close(1);close(2);for(;;)pause();}
    if (!strcmp(argv[1],"closed")) {
        int fd=atoi(argv[2]);return fcntl(fd,F_GETFD)<0&&errno==EBADF ? 0 : 85;
    }
    if (!strcmp(argv[1],"signal")) {kill(getpid(),SIGUSR1);return 0;}
    if (!strcmp(argv[1],"pause")) {for(;;)pause();}
    return 86;
}
