#include "../../runtime/process.h"
#include "../../runtime/os.h"
#include "../../runtime/io.h"
#include "../../runtime/signal.h"
#include "../../runtime/memory.h"
#define CHECK(c) do { checks++; if (!(c)) { uint64_t written; luxrt_write_full(2, #c "\n", sizeof(#c "\n")-1, &written); return __LINE__; } } while (0)
static int64_t fd_count(void) {
    int64_t count=0;
    for(int64_t fd=0;fd<512;fd++) if(luxrt_os_fcntl(fd,1,0)>=0)count++;
    return count;
}
static void caught(int number) {(void)number;}
int32_t luxrt_process_cases(const char *child, uint64_t *outchecks) {
    uint64_t checks=0, original_mask, after_mask;
    CHECK(luxrt_signal_mask(2,0,&original_mask)==0);
    int64_t initial_fds=fd_count(), pid=999;
    const char *args[4]={"process-child","exit",0,0};
    uint32_t raw=999;int32_t code=999;
    for(int i=0;i<20;i++) {
        CHECK(luxrt_process_spawn(child,args,0,-1,-1,-1,&pid)==0&&pid>0);
        CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==7&&raw==(7U<<8));
        CHECK(fd_count()==initial_fds);
    }
    CHECK(luxrt_signal_mask(2,0,&after_mask)==0&&after_mask==original_mask);
    for(int i=0;i<20;i++) {
        pid=999;
        CHECK(luxrt_process_spawn("/luxrt/does-not-exist",args,0,-1,-1,-1,&pid)==-2&&pid==999);
        CHECK(fd_count()==initial_fds);
        CHECK(luxrt_os_wait(-1,&raw,1)==-10);
    }
    CHECK(luxrt_process_spawn(child,args,0,-2,-1,-1,&pid)==-22&&pid==999);
    CHECK(luxrt_process_spawn(child,args,0,UINT64_C(4294967296),-1,-1,&pid)==-22&&pid==999);
    CHECK(luxrt_process_spawn(child,args,0,500,-1,-1,&pid)==-9&&pid==999);
    CHECK(fd_count()==initial_fds);
    CHECK(luxrt_signal_mask(2,0,&after_mask)==0&&after_mask==original_mask);
    raw=991;code=992;
    CHECK(luxrt_process_wait(0,&raw,&code)==-22&&raw==991&&code==992);
    CHECK(luxrt_process_wait(2147483647,&raw,&code)==-10&&raw==991&&code==992);
    /* A non-CLOEXEC descriptor at 400 must not survive child handoff. */
    int64_t nullfd=luxrt_os_open("/dev/null",0,0);
    CHECK(nullfd>=0);
    CHECK(luxrt_os_dup_flags(nullfd,400,0)==400);
    CHECK(luxrt_process_spawn(child,args,0,-1,-1,-1,&pid)==0);
    CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==7);
    CHECK(luxrt_os_fcntl(400,1,0)==0);
    CHECK(luxrt_os_close(400)==0&&luxrt_os_close(nullfd)==0);
    /* More than one getdents buffer and a high fd exercise fallback enumeration. */
    nullfd=luxrt_os_open("/dev/null",0,0);
    CHECK(nullfd>=0);
    for(int64_t fd=100;fd<400;fd++) CHECK(luxrt_os_dup_flags(nullfd,fd,0)==fd);
    CHECK(luxrt_os_dup_flags(nullfd,2000,0)==2000);
    CHECK(luxrt_process_spawn(child,args,0,-1,-1,-1,&pid)==0);
    CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==7);
    CHECK(luxrt_os_fcntl(2000,1,0)==0);
    CHECK(luxrt_os_close(2000)==0);
    for(int64_t fd=100;fd<400;fd++) CHECK(luxrt_os_close(fd)==0);
    CHECK(luxrt_os_close(nullfd)==0);
    CHECK(fd_count()==initial_fds);
    /* Three directions, exact argv/env, stdio duplicates clear CLOEXEC. */
    int32_t input[2],output[2],error[2];
    CHECK(luxrt_os_pipe(input,0)==0&&luxrt_os_pipe(output,0)==0&&luxrt_os_pipe(error,0)==0);
    args[1]="echo";
    const char *env[]={"LUXRT_PROCESS_KEY=value with spaces",0};
    CHECK(luxrt_process_spawn(child,args,env,input[0],output[1],error[1],&pid)==0);
    CHECK(luxrt_os_fcntl(input[0],1,0)==1&&luxrt_os_fcntl(output[1],1,0)==1);
    luxrt_os_close(input[0]);luxrt_os_close(output[1]);luxrt_os_close(error[1]);
    uint64_t count;unsigned char data[8];
    CHECK(luxrt_write_full(input[1],"DATA",4,&count)==0&&count==4);
    luxrt_os_close(input[1]);
    CHECK(luxrt_read_full(output[0],data,8,&count)==0&&count==4&&!luxrt_memcmp(data,"DATA",4));
    CHECK(luxrt_read_full(error[0],data,8,&count)==0&&count==4&&!luxrt_memcmp(data,"ERR!",4));
    CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==23);
    luxrt_os_close(output[0]);luxrt_os_close(error[0]);
    CHECK(fd_count()==initial_fds);
    /* Snapshot closed stdio without confusing the internal exec-error pipe. */
    int64_t backup=luxrt_os_fcntl(0,1030,4), flags=luxrt_os_fcntl(0,1,0);
    if(backup>=0) {
        CHECK(luxrt_os_close(0)==0);
        args[1]="closed";args[2]="0";
        CHECK(luxrt_process_spawn(child,args,0,-1,-1,-1,&pid)==0);
        CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==0);
        CHECK(luxrt_os_fcntl(0,1,0)==-9);
        pid=999;
        CHECK(luxrt_process_spawn("/luxrt/does-not-exist",args,0,-1,-1,-1,&pid)==-2&&pid==999);
        CHECK(luxrt_os_fcntl(0,1,0)==-9);
        CHECK(luxrt_os_dup_flags(backup,0,flags?524288:0)==0);
        CHECK(luxrt_os_close(backup)==0);
        CHECK(fd_count()==initial_fds);
        /* Inherited CLOEXEC stdio closes on exec, explicit redirects don't. */
        CHECK(luxrt_os_fcntl(0,2,1)==0);
        CHECK(luxrt_process_spawn(child,args,0,-1,-1,-1,&pid)==0);
        CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==0);
        CHECK(luxrt_os_fcntl(0,1,0)==1);
        CHECK(luxrt_os_fcntl(0,2,flags)==0);
    }
    args[1]="signal";args[2]=0;
    struct luxrt_signal_action old, ignore={(void(*)(int))(uintptr_t)1,0,0};
    CHECK(luxrt_signal_action(10,&ignore,&old)==0);
    struct luxrt_signal_action observed;
    CHECK(luxrt_signal_action(10,0,&observed)==0&&(uintptr_t)observed.handler==1);
    CHECK(luxrt_process_spawn(child,args,0,-1,-1,-1,&pid)==0);
    CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==0);
    struct luxrt_signal_action handler={caught,0,0};
    CHECK(luxrt_signal_action(10,&handler,0)==0);
    CHECK(luxrt_process_spawn(child,args,0,-1,-1,-1,&pid)==0);
    CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==138&&(raw&127)==10);
    CHECK(luxrt_signal_action(10,&old,0)==0);
    args[1]="pause";
    CHECK(luxrt_process_spawn(child,args,0,-1,-1,-1,&pid)==0);
    CHECK(luxrt_os_kill(pid,9)==0);
    CHECK(luxrt_process_wait(pid,&raw,&code)==0&&code==137&&(raw&127)==9);
    CHECK(fd_count()==initial_fds);
    CHECK(luxrt_signal_mask(2,0,&after_mask)==0&&after_mask==original_mask);
    CHECK(luxrt_os_wait(-1,&raw,1)==-10);
    CHECK(luxrt_process_handoff_bytes()==24);
    struct luxrt_process_handoff h={999,998,997,996};
    CHECK(luxrt_process_start(child,args,0,-1,-1,-1,2,0,&h)==-22&&h.pid==999&&h.error_fd==998);
    args[1]="exit";
    CHECK(luxrt_process_start(child,args,0,-1,-1,-1,1,0,&h)==0&&h.pid>0&&h.error_fd>=0);
    int32_t handoff_status;
    do {handoff_status=luxrt_process_handoff_poll(&h);if(handoff_status==1){int64_t delay[2]={0,1000000};luxrt_os_poll(0,0,delay);}} while(handoff_status==1);
    CHECK(handoff_status==0&&h.error_fd==-1);
    CHECK(luxrt_process_wait(h.pid,&raw,&code)==0&&code==7);
    /* Fragmented records, partial EOF, malformed status and close ownership. */
    int32_t channel[2],record=-2;
    CHECK(luxrt_os_pipe(channel,2048)==0);
    h.pid=1;h.error_fd=channel[0];h.bytes=0;h.record=0;
    CHECK(luxrt_process_handoff_poll(&h)==1&&h.bytes==0);
    CHECK(luxrt_os_write(channel[1],&record,2)==2);
    CHECK(luxrt_process_handoff_poll(&h)==1&&h.bytes==2);
    CHECK(luxrt_os_write(channel[1],(unsigned char*)&record+2,2)==2);
    CHECK(luxrt_process_handoff_poll(&h)==-2&&h.error_fd==-1);
    CHECK(luxrt_os_fcntl(channel[0],1,0)==-9);
    CHECK(luxrt_process_handoff_poll(&h)==-22);
    luxrt_os_close(channel[1]);
    CHECK(luxrt_os_pipe(channel,2048)==0);
    h.error_fd=channel[0];h.bytes=0;h.record=0;
    CHECK(luxrt_os_write(channel[1],&record,1)==1);
    luxrt_os_close(channel[1]);
    CHECK(luxrt_process_handoff_poll(&h)==1&&h.bytes==1);
    CHECK(luxrt_process_handoff_poll(&h)==-5&&h.error_fd==-1);
    CHECK(luxrt_os_pipe(channel,2048)==0);
    h.error_fd=channel[0];h.bytes=0;h.record=0;record=1;
    CHECK(luxrt_os_write(channel[1],&record,4)==4);
    CHECK(luxrt_process_handoff_poll(&h)==-5&&h.error_fd==-1);
    luxrt_os_close(channel[1]);
    CHECK(luxrt_os_pipe(channel,2048)==0);
    h.error_fd=channel[0];h.bytes=0;h.record=0;
    luxrt_os_close(channel[1]);
    CHECK(luxrt_process_handoff_poll(&h)==0&&h.error_fd==-1);
    CHECK(fd_count()==initial_fds);
    *outchecks=checks;return 0;
}
