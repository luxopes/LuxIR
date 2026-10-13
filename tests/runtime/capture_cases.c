#include "../../runtime/capture.h"
#include "../../runtime/os.h"
#include "../../runtime/signal.h"
#include "../../runtime/io.h"
#define CHECK(c) do { checks++;if(!(c)){uint64_t n;luxrt_write_full(2,#c "\n",sizeof(#c "\n")-1,&n);return __LINE__;} }while(0)
static int64_t fd_count(void){int64_t n=0;for(int fd=0;fd<512;fd++)if(luxrt_os_fcntl(fd,1,0)>=0)n++;return n;}
int32_t luxrt_capture_cases(const char *child,uint64_t *outchecks) {
 uint64_t checks=0,mask,after;int64_t fds=fd_count();
 CHECK(luxrt_signal_mask(2,0,&mask)==0);
 CHECK(luxrt_capture_request_bytes()==56&&luxrt_capture_result_bytes()==40);
 uint64_t size=UINT64_C(1048576);
 int64_t allocation=luxrt_os_map(size*4);CHECK(allocation>0);
 unsigned char *input=(unsigned char*)(uintptr_t)allocation,*output=input+size,*error=output+size*2;
 for(uint64_t i=0;i<size;i++)input[i]=(unsigned char)(i%251);
 struct luxrt_capture_request q={input,size,output,size*2,error,size,10000};
 struct luxrt_capture_result r;
 const char *args[]={"child","flood",0,0};
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==0&&r.exit_code==29&&r.raw_status==(29U<<8)&&r.pid>0);
 CHECK(r.input_sent==size&&r.output_size==size*2&&r.error_size==size);
 for(uint64_t i=0;i<size;i++){CHECK(output[i]=='P');CHECK(output[size+i]==input[i]);CHECK(error[i]=='E');}
 CHECK(fd_count()==fds);
 CHECK(luxrt_signal_mask(2,0,&after)==0&&after==mask);
 /* Exact-full output is accepted; a one-byte probe distinguishes overflow. */
 q.input_size=4;q.output_capacity=4;q.error_capacity=4;args[1]="echo";
 const char *env[]={"LUXRT_PROCESS_KEY=value with spaces",0};
 CHECK(luxrt_process_capture(child,args,env,&q,&r)==0&&r.exit_code==23);
 CHECK(r.input_sent==4&&r.output_size==4&&r.error_size==4);
 for(int i=0;i<4;i++)CHECK(output[i]==input[i]);
 q.output_capacity=3;
 CHECK(luxrt_process_capture(child,args,env,&q,&r)==-28&&r.output_size==3);
 CHECK(fd_count()==fds&&luxrt_os_wait(-1,&r.raw_status,1)==-10);
 q.input_size=size;q.output_capacity=size*2;q.error_capacity=0;args[1]="flood";
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==-28&&r.error_size==0);
 CHECK(fd_count()==fds&&luxrt_os_wait(-1,&r.raw_status,1)==-10);
 /* Closed child stdin must never deliver SIGPIPE to the parent. */
 args[1]="exit";q.error_capacity=size;
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==0&&r.exit_code==7&&r.input_sent<=size&&r.output_size==0&&r.error_size==0);
 args[1]="pause";q.input_size=0;q.timeout_ms=30;
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==-110&&r.exit_code==137&&(r.raw_status&127)==9);
 CHECK(fd_count()==fds&&luxrt_os_wait(-1,&r.raw_status,1)==-10);
 args[1]="closed-pause";
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==-110&&r.exit_code==137);
 CHECK(fd_count()==fds&&luxrt_os_wait(-1,&r.raw_status,1)==-10);
 args[1]="group";q.timeout_ms=1000;
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==0&&r.exit_code==0);
 args[1]="mask";args[2]=(mask&(UINT64_C(1)<<16))?"1":"0";
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==0&&r.exit_code==0);
 args[1]="exit";args[2]=0;
 CHECK(luxrt_process_capture("/luxrt/does-not-exist",args,0,&q,&r)==-2&&r.pid>0&&r.input_sent<=q.input_size&&r.output_size==0&&r.error_size==0);
 CHECK(fd_count()==fds);
 q.timeout_ms=UINT64_C(2147483648);
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==-22&&r.pid==0);
 q.timeout_ms=1000;q.input=0;q.input_size=1;
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==-22&&r.pid==0);
 q.input_size=0;
 struct luxrt_signal_action old,ignore={(void(*)(int))(uintptr_t)1,0,0};
 CHECK(luxrt_signal_action(17,&ignore,&old)==0);
 CHECK(luxrt_process_capture(child,args,0,&q,&r)==-22&&r.pid==0);
 CHECK(luxrt_signal_action(17,&old,0)==0);
 CHECK(luxrt_signal_mask(2,0,&after)==0&&after==mask);
 CHECK(luxrt_os_unmap((void*)(uintptr_t)allocation,size*4)==0);
 CHECK(fd_count()==fds&&luxrt_os_wait(-1,&r.raw_status,1)==-10);
 *outchecks=checks;return 0;
}
