#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern int64_t opera_numeric(const int64_t *,uint64_t,uint64_t);
extern int64_t opera_text(const unsigned char *,uint64_t,uint64_t,uint64_t);
static double now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(3);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv){
 if(argc!=4)return 2;
 uint64_t rows=strtoull(argv[2],NULL,10),cols=strtoull(argv[3],NULL,10);
 if(!cols||rows>1000000||cols>1000)return 2;
 uint64_t n=rows*cols;int64_t *values=malloc((size_t)(n+1)*8);unsigned char *text=malloc((size_t)(n+1)*5);
 if(!values||!text)return 3;
 uint32_t state=0x12345678;size_t length=0;int64_t expected=0;
 for(uint64_t r=0;r<rows;r++){
  int64_t best=-101;
  for(uint64_t c=0;c<cols;c++){
   state^=state<<13;state^=state>>17;state^=state<<5;int32_t v=(int32_t)(state%201)-100;
   /* All-negative rows ensure max cannot start at zero. */
   if(r%31==0&&v>=0)v=-v-1;
   if(v<-100)v=-100;
   values[r*cols+c]=v;if(v>best)best=v;
   unsigned magnitude=(unsigned)(v<0?-v:v);if(v<0)text[length++]='-';
   if(magnitude==100){text[length++]='1';text[length++]='0';text[length++]='0';}
   else {if(magnitude>=10)text[length++]=(unsigned char)('0'+magnitude/10);text[length++]=(unsigned char)('0'+magnitude%10);}
   text[length++]=(c+1==cols)?'\n':' ';
  }
  expected+=best;
 }
 int numeric=!strcmp(argv[1],"numeric");if(!numeric&&strcmp(argv[1],"text"))return 2;
 unsigned repetitions=numeric?16:4;int64_t result=0,checksum=0;
 double start=now();
 for(unsigned repeat=0;repeat<repetitions;repeat++){result=numeric?opera_numeric(values,rows,cols):opera_text(text,length,rows,cols);checksum+=result;}
 double seconds=(now()-start)/repetitions;
 if(checksum!=expected*repetitions)return 1;
 if(result!=expected){fprintf(stderr,"expected %"PRId64" got %"PRId64"\n",expected,result);return 1;}
 printf("%.9f,%"PRId64",%zu\n",seconds,result,length);free(values);free(text);return 0;
}
