#include "../../runtime/integer.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed at %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;uint64_t state=UINT64_C(0x123456789abcdef0);
 int64_t edges[]={0,1,-1,INT64_MIN,INT64_MAX,10,-10,999999999999999999};
 for(unsigned i=0;i<100008;i++) {
  state^=state<<13;state^=state>>7;state^=state<<17;int64_t value;memcpy(&value,&state,8);if(i<8)value=edges[i];
  char expected[32],got[32];int len=snprintf(expected,sizeof expected,"%" PRId64,value);memset(got,0xa5,sizeof got);
  CHECK(luxrt_format_i64(value,got,sizeof got)==len);CHECK(memcmp(expected,got,(size_t)len)==0);CHECK((unsigned char)got[len]==0xa5);
  int64_t parsed=77;CHECK(luxrt_parse_i64(got,(uint64_t)len,&parsed)==0);CHECK(parsed==value);
  memset(got,0xa5,sizeof got);CHECK(luxrt_format_i64(value,got,(uint64_t)len-1)==-28);for(unsigned j=0;j<sizeof got;j++)CHECK((unsigned char)got[j]==0xa5);
 }
 const char *bad[]={"","+","-"," 1","1 ","1x","--1","9223372036854775808","-9223372036854775809","18446744073709551617"};
 for(unsigned i=0;i<10;i++){int64_t out=42;CHECK(luxrt_parse_i64(bad[i],strlen(bad[i]),&out)==(i>=7?-34:-22));CHECK(out==42);}
 int64_t out=42;CHECK(luxrt_parse_i64(NULL,0,&out)==-22);CHECK(out==42);CHECK(luxrt_format_i64(INT64_MIN,NULL,0)==-28);
 CHECK(luxrt_parse_i64("+00042",6,&out)==0);CHECK(out==42);
 printf("%u decimal integer checks passed\n",checks);return 0;}
