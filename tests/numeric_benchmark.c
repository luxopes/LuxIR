#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
extern uint64_t numeric_bulk_high(const uint64_t*,uint64_t),numeric_bulk_high_fallback(const uint64_t*,uint64_t);
static double stamp(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(2);return t.tv_sec+t.tv_nsec*1e-9;}
static volatile uint64_t sink;
static double run(uint64_t (*fn)(const uint64_t*,uint64_t),const uint64_t *data,uint64_t n,uint64_t expected){double start=stamp();for(unsigned r=0;r<16;r++){uint64_t sum=fn(data,n);if(sum!=expected)exit(3);sink=sum;}return (stamp()-start)/16;}
int main(void){uint64_t n=1000000,*data=malloc(16*n),state=0x7346372819,expected=0;if(!data)return 2;
 for(uint64_t i=0;i<2*n;i++){state^=state<<13;state^=state>>7;state^=state<<17;data[i]=state;}
 for(uint64_t i=0;i<n;i++)expected+=(uint64_t)(((__uint128_t)data[2*i]*data[2*i+1])>>64);
 run(numeric_bulk_high,data,n,expected);run(numeric_bulk_high_fallback,data,n,expected);
 puts("intrinsic_seconds,fallback_seconds");for(unsigned trial=0;trial<7;trial++){double a,b;if(trial&1){b=run(numeric_bulk_high_fallback,data,n,expected);a=run(numeric_bulk_high,data,n,expected);}else{a=run(numeric_bulk_high,data,n,expected);b=run(numeric_bulk_high_fallback,data,n,expected);}printf("%.9f,%.9f\n",a,b);}free(data);return 0;
}
