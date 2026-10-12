#include "../../runtime/relocate.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned protects;static void *protected_address;static uint64_t protected_size;static int64_t protect_result;
int64_t luxrt_os_protect(void *p,uint64_t size,uint64_t flags){if(flags!=1)abort();protects++;protected_address=p;protected_size=size;return protect_result;}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
int main(void){unsigned checks=0;unsigned char *arena=aligned_alloc(4096,16384);CHECK(arena!=NULL);memset(arena,0,16384);
 uint64_t ph[14]={0},stack[]={0,0,0,3,(uintptr_t)ph,4,56,5,2,6,4096,0,0};ph[7]=0x6474e552;ph[9]=6144;ph[12]=2048;
 uint64_t dynamic[]={7,64,8,48,9,24,0,0},*rela=(void*)(arena+64),*target=(void*)(arena+512);rela[0]=512;rela[2]=9000;rela[3]=520;rela[4]=0;rela[5]=10000;
 uint64_t types[]={8,1027,3};for(unsigned n=0;n<3;n++){rela[1]=types[n];protects=0;CHECK(luxrt_startup_relocate(stack,arena,dynamic,types[n])==0);CHECK(*target==(uintptr_t)(arena+9000));CHECK(*(uint64_t*)(arena+520)==0);CHECK(protects==1&&protected_address==arena+4096&&protected_size==4096);}
 rela[1]=8;dynamic[3]=25;CHECK(luxrt_startup_relocate(stack,arena,dynamic,8)==-22);dynamic[3]=48;
 dynamic[5]=32;CHECK(luxrt_startup_relocate(stack,arena,dynamic,8)==-22);dynamic[5]=24;
 dynamic[1]=0;CHECK(luxrt_startup_relocate(stack,arena,dynamic,8)==-22);dynamic[1]=64;
 rela[1]=9;CHECK(luxrt_startup_relocate(stack,arena,dynamic,8)==-95);rela[1]=UINT64_C(1)<<32|8;CHECK(luxrt_startup_relocate(stack,arena,dynamic,8)==-95);rela[1]=8;
 uint64_t unsupported[]={17,1,0,0};CHECK(luxrt_startup_relocate(stack,arena,unsupported,8)==-95);
 stack[6]=32;CHECK(luxrt_startup_relocate(stack,arena,dynamic,8)==-22);stack[6]=56;
 stack[10]=3;CHECK(luxrt_startup_relocate(stack,arena,dynamic,8)==-22);stack[10]=4096;
 protect_result=-13;CHECK(luxrt_startup_relocate(stack,arena,dynamic,8)==-13);protect_result=0;
 uint64_t empty[]={0,0};CHECK(luxrt_startup_relocate(stack,arena,empty,8)==0);
 free(arena);printf("%u PIE relocation/RELRO checks passed (three relative types, invalid tables and protect failure)\n",checks);return 0;
}
