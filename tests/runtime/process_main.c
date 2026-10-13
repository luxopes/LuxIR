#include <stdint.h>
#include <stdio.h>
int32_t luxrt_process_cases(const char *,uint64_t *);
int main(int argc,char **argv) {
 if(argc!=2)return 1;
 uint64_t checks=0;int32_t failure=luxrt_process_cases(argv[1],&checks);
 if(failure){fprintf(stderr,"process failure line %d\n",failure);return 1;}
 printf("%lu spawn/wait checks passed\n",checks);return 0;
}
