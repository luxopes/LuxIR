#include "../../runtime/checked.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;uint64_t seed=UINT64_C(0x123456789abcdef0);int64_t edges[]={INT64_MIN,INT64_MAX,0,1,-1,2,-2,3037000500};
 for(unsigned i=0;i<100064;i++) {
  int64_t a,b;seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;memcpy(&a,&seed,8);seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;memcpy(&b,&seed,8);if(i<64){a=edges[i/8];b=edges[i%8];}
  __int128 expected[3]={(__int128)a+b,(__int128)a-b,(__int128)a*b};int32_t(*ops[3])(int64_t,int64_t,int64_t*)={luxrt_checked_add,luxrt_checked_sub,luxrt_checked_mul};
  for(unsigned k=0;k<3;k++){int64_t out=42;int status=ops[k](a,b,&out);if(expected[k]<INT64_MIN||expected[k]>INT64_MAX){CHECK(status==-34);CHECK(out==42);}else{CHECK(status==0);CHECK(out==(int64_t)expected[k]);}}
  int64_t out=42;int status=luxrt_checked_floordiv(a,b,&out);
  if(!b){CHECK(status==-33);CHECK(out==42);}else if(a==INT64_MIN&&b==-1){CHECK(status==-34);CHECK(out==42);}else{__int128 q=(__int128)a/b,r=(__int128)a%b;if(r&&((a<0)!=(b<0)))q--;CHECK(status==0);CHECK(out==(int64_t)q);}
 }
 printf("%u checked arithmetic checks passed\n",checks);return 0;}
