#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int64_t opera_numeric(const int64_t *,uint64_t,uint64_t);
extern int64_t opera_text(const unsigned char *,uint64_t,uint64_t,uint64_t);
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d\n",__LINE__);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;int64_t example[]={10,25,-5,18,-3,-10,-5,-2,80,12,40,95};
 const char *sample="10 25 -5 18\n-3 -10 -5 -2\n80 12 40 95\n";
 CHECK(opera_numeric(example,3,4)==118);CHECK(opera_text((const unsigned char*)sample,strlen(sample),3,4)==118);
 int64_t negative[]={-100,-100,-1,-2};CHECK(opera_numeric(negative,2,2)==-101);
 CHECK(opera_text((const unsigned char*)"-100\t-100\r\n-1 -2",16,2,2)==-101);
 CHECK(opera_numeric(NULL,0,1)==0);CHECK(opera_text(NULL,0,0,1)==0);
 const char *bad[]={"","-","101","-101","+1","a","1x","1 -"};
 for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)CHECK(opera_text((const unsigned char*)bad[i],strlen(bad[i]),1,i==7?2:1)==INT64_MIN);
 printf("%u opera correctness checks passed\n",checks);return 0;}
