#include "../../runtime/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"arena failure %d: %s\n",__LINE__,#c); exit(1); } checks++; } while(0)
static uint64_t seed=UINT64_C(0x58393210bcfe);
static uint64_t random64(void) { seed ^= seed<<13; seed ^= seed>>7; seed ^= seed<<17; return seed; }
int main(void) {
    unsigned checks=0;
    _Alignas(4096) unsigned char buffer[4096+32];
    unsigned char expected[sizeof buffer];
    luxrt_arena arena, saved;
    memset(&arena,0x5a,sizeof arena); saved=arena;
    CHECK(luxrt_arena_init(&arena,NULL,1)==-22); CHECK(!memcmp(&arena,&saved,sizeof arena));
    CHECK(luxrt_arena_init(&arena,(void *)(uintptr_t)(UINT64_MAX-3),5)==-75); CHECK(!memcmp(&arena,&saved,sizeof arena));
    CHECK(luxrt_arena_init(&arena,NULL,0)==0);
    void *out=(void *)(uintptr_t)42;
    CHECK(luxrt_arena_alloc(&arena,1,1,&out)==-12); CHECK(out==(void *)(uintptr_t)42 && arena.used==0);
    CHECK(luxrt_arena_rewind(&arena,1)==-22); CHECK(arena.used==0);
    CHECK(sizeof arena==24);
    for (unsigned cycle=0;cycle<2048;cycle++) {
        memset(buffer,0xcc,sizeof buffer); memcpy(expected,buffer,sizeof buffer);
        unsigned skew=(unsigned)(random64()%16)+8;
        uint64_t capacity=random64()%4097;
        unsigned char *base=buffer+skew;
        CHECK(luxrt_arena_init(&arena,base,capacity)==0);
        uint64_t used=0;
        for (unsigned i=0;i<100;i++) {
            uint64_t size=random64()%129, align=UINT64_C(1)<<(random64()%13);
            if (i%17==0) align=3;
            if (i%19==0) align=0;
            if (i%23==0) size=UINT64_MAX;
            __uint128_t rounded=((__uint128_t)(uintptr_t)base+used+align-1);
            if (align) rounded=rounded/align*align;
            __uint128_t next=rounded-(uintptr_t)base+size;
            int status=(!size||!align||(align&(align-1)))?-22:next>capacity?-12:0;
            saved=arena;out=(void *)(uintptr_t)42;
            CHECK(luxrt_arena_alloc(&arena,size,align,&out)==status);
            if(status) { CHECK(out==(void *)(uintptr_t)42);CHECK(!memcmp(&arena,&saved,sizeof arena)); }
            else {
                CHECK((uintptr_t)out==(uintptr_t)rounded);CHECK(arena.used==(uint64_t)next);
                CHECK(!memcmp(buffer,expected,sizeof buffer)); /* allocation must not clear bytes */
                memset(out,(int)(i&255),(size_t)size);
                memset(expected+skew+(uint64_t)(rounded-(uintptr_t)base),(int)(i&255),(size_t)size);
                used=(uint64_t)next;
            }
            if(i%11==0) {
                CHECK(luxrt_arena_rewind(&arena,0)==0); used=0;
                saved=arena; CHECK(luxrt_arena_rewind(&arena,UINT64_MAX)==-22);CHECK(!memcmp(&arena,&saved,sizeof arena));
            }
            CHECK(arena.used==used && arena.base==base && arena.capacity==capacity);
            CHECK(!memcmp(buffer,expected,sizeof buffer)); /* includes both end canaries */
        }
    }
    /* Alignment and size with bit 63 set must be treated as unsigned. */
    CHECK(luxrt_arena_init(&arena,buffer,sizeof buffer)==0);
    saved=arena;out=(void *)(uintptr_t)42;
    CHECK(luxrt_arena_alloc(&arena,1,UINT64_C(1)<<63,&out)==-12);
    CHECK(out==(void *)(uintptr_t)42 && !memcmp(&arena,&saved,sizeof arena));
    CHECK(luxrt_arena_alloc(&arena,UINT64_MAX,1,&out)==-12);
    CHECK(out==(void *)(uintptr_t)42 && !memcmp(&arena,&saved,sizeof arena));
    printf("%u arena checks passed\n",checks);
    return 0;
}
