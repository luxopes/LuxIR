#define _POSIX_C_SOURCE 200809L
#include "../../runtime/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define N 1000000
#define REPEATS 16
static double now(void) { struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) abort(); return t.tv_sec+t.tv_nsec*1e-9; }
int main(void) {
    unsigned char *base=malloc(16*N+16);
    if(!base) return 1;
    double times[7];
    for(unsigned trial=0;trial<8;trial++) {
        luxrt_arena arena; void *out=NULL;
        if(luxrt_arena_init(&arena,base,16*N+16)) abort();
        uint64_t checksum=0;
        double start=now();
        for(unsigned repeat=0;repeat<REPEATS;repeat++) {
          if(luxrt_arena_rewind(&arena,0)) abort();
          for(unsigned i=0;i<N;i++) {
            if(luxrt_arena_alloc(&arena,16,16,&out)) abort();
            checksum+=(uintptr_t)out-(uintptr_t)base;
          }
        }
        double elapsed=now()-start;
        uint64_t padding=(0-(uintptr_t)base)&15;
        if(checksum!=REPEATS*(UINT64_C(16)*N*(N-1)/2+padding*N) || arena.used!=16*N+padding) abort();
        if(trial)times[trial-1]=elapsed*1e9/(N*REPEATS);
    }
    for(unsigned i=0;i<7;i++)for(unsigned j=i+1;j<7;j++)if(times[j]<times[i]){double t=times[i];times[i]=times[j];times[j]=t;}
    printf("{\"allocations_per_trial\":%d,\"trials\":7,\"median_ns_per_allocation\":%.6f,\"managed_gc\":false,\"touch_payload\":false}\n",N*REPEATS,times[3]);
    free(base);return 0;
}
