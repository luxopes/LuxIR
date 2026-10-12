#define _GNU_SOURCE
#include "../../runtime/io.h"
#include "../../runtime/os.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
static volatile sig_atomic_t interrupted;
static void alarm_handler(int number){(void)number;interrupted++;}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;uint64_t count=99;CHECK(luxrt_read_full(-1,NULL,0,&count)==0&&count==0);CHECK(luxrt_write_full(-1,NULL,0,&count)==0&&count==0);
 char buffer[16];CHECK(luxrt_read_full(-1,buffer,1,&count)==-EBADF&&count==0);CHECK(luxrt_write_full(-1,buffer,1,&count)==-EBADF&&count==0);
 CHECK(luxrt_read_full(-1,buffer,UINT64_MAX,&count)==-ERANGE&&count==0);CHECK(luxrt_write_full(-1,buffer,UINT64_MAX,&count)==-ERANGE&&count==0);
 int pipes[2];CHECK(pipe2(pipes,O_NONBLOCK|O_CLOEXEC)==0);CHECK(write(pipes[1],"short",5)==5);
 CHECK(luxrt_read_full(pipes[0],buffer,16,&count)==-EAGAIN&&count==5);CHECK(!memcmp(buffer,"short",5));CHECK(close(pipes[1])==0);
 CHECK(luxrt_read_full(pipes[0],buffer,16,&count)==0&&count==0);CHECK(close(pipes[0])==0);
 size_t n=300001;char *sent=malloc(n),*received=malloc(n);CHECK(sent&&received);for(size_t i=0;i<n;i++)sent[i]=(char)(i*37);
 CHECK(pipe2(pipes,O_CLOEXEC)==0);pid_t child=fork();CHECK(child>=0);
 if(!child){close(pipes[0]);uint64_t written=0;int64_t rc=luxrt_write_full(pipes[1],sent,n,&written);close(pipes[1]);_exit(rc||written!=n);}
 CHECK(close(pipes[1])==0);CHECK(luxrt_read_full(pipes[0],received,n,&count)==0&&count==n);CHECK(!memcmp(sent,received,n));CHECK(close(pipes[0])==0);
 int status=0;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 struct sigaction action={0};action.sa_handler=alarm_handler;sigemptyset(&action.sa_mask);CHECK(sigaction(SIGALRM,&action,NULL)==0);
 CHECK(pipe2(pipes,O_CLOEXEC)==0);child=fork();CHECK(child>=0);
 if(!child){close(pipes[0]);usleep(40000);int ok=write(pipes[1],"signal",6)==6;close(pipes[1]);_exit(!ok);}
 CHECK(close(pipes[1])==0);struct itimerval timer={{0,0},{0,10000}};CHECK(setitimer(ITIMER_REAL,&timer,NULL)==0);
 CHECK(luxrt_read_full(pipes[0],buffer,16,&count)==0&&count==6);CHECK(interrupted>0);CHECK(!memcmp(buffer,"signal",6));CHECK(close(pipes[0])==0);
 CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 free(sent);free(received);printf("%u full I/O checks passed (300001-byte pipe, short reads, partial error)\n",checks);return 0;}
