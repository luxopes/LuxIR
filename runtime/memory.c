#include "memory.h"
#include <string.h>
/* Independently compiled reference; native LuxIR implementation is memory.lir. */
void *luxrt_memcpy(void *d,const void *s,uint64_t n) { return n ? memcpy(d,s,n) : d; }
void *luxrt_memmove(void *d,const void *s,uint64_t n) { return n ? memmove(d,s,n) : d; }
void *luxrt_memset(void *d,uint32_t b,uint64_t n) { return n ? memset(d,(unsigned char)b,n) : d; }
int32_t luxrt_memcmp(const void *a,const void *b,uint64_t n) {
    if (!n) return 0;
    int c=memcmp(a,b,n); return (c>0)-(c<0);
}
void *luxrt_memchr(const void *p,uint32_t b,uint64_t n) { return n ? memchr(p,(unsigned char)b,n) : NULL; }
uint32_t luxrt_hash_bytes(const void *p,uint64_t n) {
    const unsigned char *s=p;uint32_t h=2166136261u;
    for(uint64_t i=0;i<n;i++) h=(h^s[i])*16777619u;
    return h;
}
int32_t luxrt_utf8_valid(const void *p,uint64_t n) {
    const unsigned char *s=p;uint64_t i=0;
    while(i<n) {
        unsigned c=s[i++], count, min=128,max=191;
        if(c<128) continue;
        if(c>=194&&c<=223) count=1;
        else if(c>=224&&c<=239) {count=2;if(c==224)min=160;if(c==237)max=159;}
        else if(c>=240&&c<=244) {count=3;if(c==240)min=144;if(c==244)max=143;}
        else return 0;
        if(n-i<count || s[i]<min || s[i]>max) return 0;
        i++;
        for(unsigned j=1;j<count;j++,i++) if(s[i]<128||s[i]>191)return 0;
    }
    return 1;
}
