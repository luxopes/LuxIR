#include "integer.h"
#include <limits.h>
int32_t luxrt_parse_i64(const void *text,uint64_t n,int64_t *output) {
 const unsigned char *s=text;uint64_t i=0,value=0;int negative=0;
 if(!n)return -22;
 if(s[0]=='-'||s[0]=='+'){negative=s[0]=='-';i++;}
 if(i==n)return -22;
 for(;i<n;i++) {
  unsigned digit=(unsigned)s[i]-'0';if(digit>9)return -22;
  if(value>UINT64_C(922337203685477580)||(value==UINT64_C(922337203685477580)&&digit>(negative?8u:7u)))return -34;
  value=value*10+digit;
 }
 *output=negative?(value==UINT64_C(9223372036854775808)?INT64_MIN:-(int64_t)value):(int64_t)value;
 return 0;
}
int64_t luxrt_format_i64(int64_t value,void *buffer,uint64_t capacity) {
 unsigned char digits[20],*out=buffer;unsigned count=0;int negative=value<0;
 uint64_t magnitude=negative?UINT64_C(0)-(uint64_t)value:(uint64_t)value;
 do{digits[count++]=(unsigned char)('0'+magnitude%10);magnitude/=10;}while(magnitude);
 unsigned total=count+(unsigned)negative;if(capacity<total)return -28;
 unsigned i=0;if(negative)out[i++]='-';while(count)out[i++]=digits[--count];return total;
}
