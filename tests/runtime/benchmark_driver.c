#define _POSIX_C_SOURCE 200809L
#include "../../runtime/memory.h"
#include "../../runtime/integer.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(1);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv){if(argc!=2)return 2;unsigned char text[64];memset(text,'x',sizeof text);char formatted[32];uint64_t checksum=0;double start=now();
 if(!strcmp(argv[1],"hash"))for(unsigned i=0;i<1000000;i++)checksum+=luxrt_hash_bytes(text,sizeof text);
 else if(!strcmp(argv[1],"utf8"))for(unsigned i=0;i<1000000;i++)checksum+=(unsigned)luxrt_utf8_valid(text,sizeof text);
 else if(!strcmp(argv[1],"format"))for(unsigned i=0;i<1000000;i++)checksum+=(uint64_t)luxrt_format_i64(INT64_MIN+(int64_t)i,formatted,sizeof formatted);
 else return 2;
 double elapsed=now()-start;printf("%.9f,%" PRIu64 "\n",elapsed,checksum);return 0;}
