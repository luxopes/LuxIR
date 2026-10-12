#include <stdint.h>
#include <stddef.h>
/* Typed numeric kernel. Caller provides rows*columns elements, columns>0,
 * values -100..100, rows<=INT64_MAX/100. Negative rows still select one. */
int64_t opera_numeric(const int64_t *values,uint64_t rows,uint64_t columns){
 int64_t total=0;
 for(uint64_t row=0;row<rows;row++){
  int64_t best=-101;
  for(uint64_t column=0;column<columns;column++){
   int64_t value=*values++;
   if(value>best)best=value;
  }
  total+=best;
 }
 return total;
}
/* Bounded streaming parser, same trusted contest domain. No token objects,
 * dictionaries or managed GC. Header is decoded outside this benchmark.
 * Bounds/invalid tokens return INT64_MIN, impossible for valid benchmark rows. */
int64_t opera_text(const unsigned char *text,uint64_t length,uint64_t rows,uint64_t columns){
 uint64_t index=0;int64_t total=0;
 for(uint64_t row=0;row<rows;row++){
  int64_t best=-101;
  for(uint64_t column=0;column<columns;column++){
   while(index<length&&(text[index]==' '||(text[index]>=9&&text[index]<=13)))index++;
   if(index==length)return INT64_MIN;
   int negative=text[index]=='-';if(negative)index++;
   uint32_t value=0;unsigned digits=0;
   while(index<length&&text[index]>='0'&&text[index]<='9'){
    value=value*10+(uint32_t)(text[index++]-'0');digits++;
    if(value>100)return INT64_MIN;
   }
   if(!digits)return INT64_MIN;
   if(index<length&&text[index]!=' '&&(text[index]<9||text[index]>13))return INT64_MIN;
   int64_t signed_value=negative?-(int64_t)value:(int64_t)value;
   if(signed_value>best)best=signed_value;
  }
  total+=best;
 }
 return total;
}
