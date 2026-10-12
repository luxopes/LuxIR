#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <fenv.h>
extern uint64_t numeric_umulh(uint64_t,uint64_t),numeric_mulh(uint64_t,uint64_t);
extern uint64_t numeric_clz64(uint64_t),numeric_ctz64(uint64_t),numeric_popcount64(uint64_t);
extern uint32_t numeric_clz32(uint32_t),numeric_ctz32(uint32_t),numeric_popcount32(uint32_t);
extern double numeric_sqrt64(double);
extern float numeric_sqrt32(float);
extern uint64_t numeric_stress(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static uint64_t checks,rng=UINT64_C(0x973892847234);
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s (check %llu)\n",__LINE__,#c,(unsigned long long)checks);exit(1);}checks++;}while(0)
static uint64_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static unsigned pc(uint64_t x){unsigned n=0;while(x){n+=(unsigned)(x&1);x>>=1;}return n;}
static unsigned clz(uint64_t x,unsigned w){unsigned n=0;for(unsigned i=w;i>0;i--){if(x&(UINT64_C(1)<<(i-1)))break;n++;}return n;}
static unsigned ctz(uint64_t x,unsigned w){unsigned n=0;while(n<w&&!(x&(UINT64_C(1)<<n)))n++;return n;}
static uint64_t uh(uint64_t x,uint64_t y){return (uint64_t)(((__uint128_t)x*y)>>64);}
static uint64_t sh(uint64_t x,uint64_t y){return (uint64_t)(((__int128)(int64_t)x*(int64_t)y)>>64);}
static void integer(uint64_t x,uint64_t y){
 CHECK(numeric_umulh(x,y)==uh(x,y));CHECK(numeric_mulh(x,y)==sh(x,y));
 CHECK(numeric_clz64(x)==clz(x,64));CHECK(numeric_ctz64(x)==ctz(x,64));CHECK(numeric_popcount64(x)==pc(x));
 CHECK(numeric_clz32((uint32_t)x)==clz((uint32_t)x,32));CHECK(numeric_ctz32((uint32_t)x)==ctz((uint32_t)x,32));CHECK(numeric_popcount32((uint32_t)x)==pc((uint32_t)x));
 uint64_t c=next(),d=next(),e=next(),f=next(),g=next(),h=next();
 CHECK(numeric_stress(x,y,c,d,e,f,g,h)==uh(x,y)+sh(c,d)+uh(e,e)+sh(f,UINT64_MAX)+pc(g)+clz(h,64)+ctz(h,64));
}
static void floating(uint64_t bits,uint32_t bits32){double x;float y;memcpy(&x,&bits,8);memcpy(&y,&bits32,4);
 volatile double vx=x;volatile float vy=y;double a=numeric_sqrt64(x),b=sqrt(vx);float c=numeric_sqrt32(y),d=sqrtf(vy);
 uint64_t ab,bb;uint32_t cb,db;memcpy(&ab,&a,8);memcpy(&bb,&b,8);memcpy(&cb,&c,4);memcpy(&db,&d,4);
 CHECK((isnan(a)&&isnan(b))||ab==bb);CHECK((isnan(c)&&isnan(d))||cb==db);
}
int main(void){
 uint64_t edges[]={0,1,2,3,UINT32_MAX,UINT64_MAX,UINT64_C(1)<<63,(UINT64_C(1)<<63)-1,0x5555555555555555,0xaaaaaaaaaaaaaaaa};
 for(unsigned i=0;i<sizeof edges/sizeof *edges;i++)for(unsigned j=0;j<sizeof edges/sizeof *edges;j++)integer(edges[i],edges[j]);
 for(unsigned i=0;i<64;i++){integer(UINT64_C(1)<<i,UINT64_MAX);integer(~(UINT64_C(1)<<i),UINT64_C(1)<<i);}
 for(unsigned i=0;i<100000;i++){uint64_t x=next(),y=next();integer(x,y);floating(x,(uint32_t)y);}
 int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
 for(unsigned m=0;m<4;m++){CHECK(fesetround(modes[m])==0);
  uint64_t de[]={0,UINT64_C(1)<<63,1,0x000fffffffffffff,0x0010000000000000,0x3ff0000000000000,0x4000000000000000,0x7fefffffffffffff,0x7ff0000000000000,0xfff0000000000000,0x7ff8000000000001};
  uint32_t fe[]={0,UINT32_C(1)<<31,1,0x007fffff,0x00800000,0x3f800000,0x40000000,0x7f7fffff,0x7f800000,0xff800000,0x7fc00001};
  for(unsigned i=0;i<sizeof de/sizeof *de;i++)floating(de[i],fe[i]);
  for(unsigned i=0;i<1000;i++){uint64_t x=next();floating(x&~(UINT64_C(1)<<63),(uint32_t)x&~(UINT32_C(1)<<31));}
 }
 CHECK(fesetround(FE_TONEAREST)==0);
 printf("%llu numeric checks passed (128-bit oracle, 32/64-bit counts, FP edge cases and four rounding modes)\n",(unsigned long long)checks);return 0;
}
