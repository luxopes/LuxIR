#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
    if (!strcmp(argv[1],"closed")) {
        int fd=atoi(argv[2]);return fcntl(fd,F_GETFD)<0&&errno==EBADF ? 0 : 85;
    }
    if (!strcmp(argv[1],"signal")) {kill(getpid(),SIGUSR1);return 0;}
    if (!strcmp(argv[1],"pause")) {for(;;)pause();}
    return 86;
}
