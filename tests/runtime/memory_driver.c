#define _GNU_SOURCE
#include "../../runtime/memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"failed at %d: %s\n",__LINE__,#x);exit(1);}checks++; } while(0)
static uint64_t rng=UINT64_C(0x123456789abcdef0);
static unsigned char random_byte(void) {rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (unsigned char)rng;}
static uint32_t reference_hash(const unsigned char *p,size_t n) {uint32_t h=2166136261u;for(size_t i=0;i<n;i++)h=(h^p[i])*16777619u;return h;}
int main(void) {
    unsigned char src[1024],got[1024],want[1024];
    for(size_t i=0;i<sizeof src;i++)src[i]=random_byte();
    CHECK(luxrt_memcpy(NULL,NULL,0)==NULL);CHECK(luxrt_memmove(NULL,NULL,0)==NULL);
    CHECK(luxrt_memset(NULL,0,0)==NULL);CHECK(luxrt_memcmp(NULL,NULL,0)==0);
    CHECK(luxrt_memchr(NULL,0,0)==NULL);CHECK(luxrt_utf8_valid(NULL,0)==1);
    CHECK(luxrt_hash_bytes(NULL,0)==2166136261u);
    for(size_t a=0;a<16;a++)for(size_t b=0;b<16;b++)for(size_t n=0;n<128;n++) {
        memset(got,0xa5,sizeof got);memcpy(want,got,sizeof got);memcpy(want+a,src+b,n);
        CHECK(luxrt_memcpy(got+a,src+b,n)==got+a);CHECK(memcmp(got,want,sizeof got)==0);
    }
    for(size_t a=0;a<24;a++)for(size_t b=0;b<24;b++)for(size_t n=0;n<128;n++) {
        memcpy(got,src,sizeof got);memcpy(want,src,sizeof got);memmove(want+a,want+b,n);
        CHECK(luxrt_memmove(got+a,got+b,n)==got+a);CHECK(memcmp(got,want,sizeof got)==0);
    }
    for(size_t a=0;a<16;a++)for(size_t n=0;n<256;n++) {
        memset(got,0xa5,sizeof got);memcpy(want,got,sizeof got);memset(want+a,0x83,n);
        CHECK(luxrt_memset(got+a,0x183,n)==got+a);CHECK(memcmp(got,want,sizeof got)==0);
        int cmp=memcmp(src+a,want+a,n);
        CHECK(luxrt_memcmp(src+a,want+a,n)==(cmp>0)-(cmp<0));
        CHECK(luxrt_memchr(src+a,0x183,n)==memchr(src+a,0x83,n));
        CHECK(luxrt_hash_bytes(src+a,n)==reference_hash(src+a,n));
    }
    CHECK(luxrt_hash_bytes("hello",5)==0x4f9f2cabu);
    unsigned char sequences[][4]={{0xc2,0x80},{0xe0,0xa0,0x80},{0xed,0x9f,0xbf},{0xf0,0x90,0x80,0x80},{0xf4,0x8f,0xbf,0xbf},{0xc0,0x80},{0xed,0xa0,0x80},{0xf4,0x90,0x80,0x80},{0x80},{0xff}};
    unsigned length[]={2,3,3,4,4,2,3,4,1,1};
    for(unsigned i=0;i<10;i++) {CHECK(luxrt_utf8_valid(sequences[i],length[i])==(i<5));if(i<5)for(unsigned n=1;n<length[i];n++)CHECK(luxrt_utf8_valid(sequences[i],n)==0);}
    /* Guard pages make any overread/overwrite immediately fault. */
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *mapping=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED);CHECK(mprotect(mapping+page,page,PROT_READ|PROT_WRITE)==0);
    unsigned char *end=mapping+page*2;
    for(size_t n=0;n<256;n++) {
        memset(end-n,0x7f,n);CHECK(luxrt_memcpy(got,end-n,n)==got);
        CHECK(luxrt_memcpy(end-n,src,n)==end-n);
        CHECK(luxrt_memset(end-n,0x7f,n)==end-n);
        CHECK(luxrt_memcmp(end-n,end-n,n)==0);
        CHECK(luxrt_memchr(end-n,0x80,n)==NULL);
        CHECK(luxrt_hash_bytes(end-n,n)==reference_hash(end-n,n));
        CHECK(luxrt_utf8_valid(end-n,n)==1);
    }
    CHECK(munmap(mapping,page*3)==0);
    printf("%u memory/UTF-8 checks passed\n",checks);
    return 0;
}
