#include "../../runtime/managed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"managed failure line %d: %s\n",__LINE__,#c);exit(1);}checks++;}while(0)
static unsigned checks;
static uint64_t seed=UINT64_C(0x2e48371065aba);
static uint64_t random64(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
extern uint64_t test_mapped_bytes,test_map_calls;
extern int test_fail_map,test_fail_unmap;
static const uint64_t offsets[]={0,8};
static const luxrt_descriptor node={256,2,offsets,0,0,0};
int main(void) {
 luxrt_heap heap,before;memset(&heap,0x5a,sizeof heap);before=heap;
 CHECK(sizeof heap==336 && sizeof(luxrt_object)==48 && sizeof(luxrt_descriptor)==48 && sizeof(luxrt_roots)==24);
 CHECK(luxrt_heap_init(&heap,0,4096)==-22);CHECK(!memcmp(&heap,&before,sizeof heap));
 CHECK(luxrt_heap_init(&heap,65536,3)==-22);CHECK(!memcmp(&heap,&before,sizeof heap));
 CHECK(luxrt_heap_init(&heap,4*1024*1024,4096)==0);
 luxrt_value root[4]={0};luxrt_roots frame,inner;
 CHECK(luxrt_roots_push(&heap,&frame,root,4)==0);
 CHECK(luxrt_roots_push(&heap,&frame,root,4)==-22);
 CHECK(luxrt_roots_push(&heap,&inner,NULL,1)==-22);
 CHECK(luxrt_roots_push(&heap,&inner,NULL,0)==0);
 CHECK(luxrt_roots_pop(&heap,&frame)==-22);CHECK(luxrt_roots_pop(&heap,&inner)==0);
 CHECK(luxrt_heap_destroy(&heap)==-16);
 CHECK(luxrt_value_type(LUXRT_NONE)==2 && luxrt_value_type(LUXRT_TRUE)==3 && luxrt_value_type(LUXRT_FALSE)==3);
 CHECK(luxrt_value_type(0)==0 && luxrt_value_type(7)==0);
 int64_t edges[]={INT64_MIN,INT64_MAX,-1152921504606846976L,1152921504606846975L,-1152921504606846977L,1152921504606846976L,-1,0,1};
 for(unsigned i=0;i<100009;i++) {
  uint64_t bits=random64();int64_t x;memcpy(&x,&bits,8);if(i<9)x=edges[i];
  CHECK(luxrt_value_int(&heap,x,root)==0);CHECK(luxrt_value_type(root[0])==LUXRT_INT_TYPE);
  int64_t out=42;CHECK(luxrt_value_get_int(root[0],&out)==0);CHECK(out==x);
  CHECK(((root[0]&7)==1)==(x>=-1152921504606846976L && x<=1152921504606846975L));
  bits=random64();CHECK(luxrt_value_float_bits(&heap,bits,root+1)==0);uint64_t f=42;
  CHECK(luxrt_value_type(root[1])==LUXRT_FLOAT_TYPE);CHECK(luxrt_value_get_float_bits(root[1],&f)==0);CHECK(f==bits);
  CHECK(luxrt_value_get_int(root[1],&out)==-22 && out==x);
  f=42;CHECK(luxrt_value_get_float_bits(root[0],&f)==-22 && f==42);
 }
 root[0]=0;root[1]=0;CHECK(luxrt_heap_collect(&heap)==0);CHECK(heap.allocated_bytes==0 && heap.live_bytes==0);
 CHECK(luxrt_roots_pop(&heap,&frame)==0);CHECK(luxrt_heap_destroy(&heap)==0);CHECK(test_mapped_bytes==0);
 /* Exact reachability oracle for randomly connected graphs, including cycles. */
 enum{N=1024};luxrt_value values[N],roots[16];unsigned child[N][2],queue[N];unsigned char live[N];
 for(unsigned trial=0;trial<32;trial++) {
  memset(values,0,sizeof values);CHECK(luxrt_heap_init(&heap,4*1024*1024,4096)==0);CHECK(luxrt_roots_push(&heap,&frame,values,N)==0);
  for(unsigned i=0;i<N;i++) {CHECK(luxrt_object_new(&heap,&node,24,values+i)==0);uint64_t *p=luxrt_object_payload(values[i]);p[2]=i;}
  for(unsigned i=0;i<N;i++) {uint64_t *p=luxrt_object_payload(values[i]);for(unsigned k=0;k<2;k++) {unsigned j=(unsigned)(random64()%N);child[i][k]=j;p[k]=values[j];}}
  CHECK(luxrt_roots_pop(&heap,&frame)==0);memset(live,0,sizeof live);unsigned begin=0,end=0;
  for(unsigned i=0;i<16;i++) {unsigned id=(unsigned)(random64()%N);roots[i]=values[id];if(!live[id]){live[id]=1;queue[end++]=id;}}
  while(begin<end){unsigned id=queue[begin++];for(unsigned k=0;k<2;k++){unsigned j=child[id][k];if(!live[j]){live[j]=1;queue[end++]=j;}}}
  CHECK(luxrt_roots_push(&heap,&frame,roots,16)==0);CHECK(luxrt_heap_collect(&heap)==0);CHECK(heap.live_bytes==(uint64_t)end*128);
  unsigned found=0;for(luxrt_object *o=heap.objects;o;o=o->next){uint64_t *p=luxrt_object_payload((uintptr_t)o);CHECK(p[2]<N && live[p[2]]);CHECK(p[0]==values[child[p[2]][0]] && p[1]==values[child[p[2]][1]]);found++;}CHECK(found==end);
  memset(roots,0,sizeof roots);CHECK(luxrt_heap_collect(&heap)==0);CHECK(!heap.objects && !heap.live_bytes);
  CHECK(luxrt_roots_pop(&heap,&frame)==0);memset(values,0,sizeof values);CHECK(luxrt_roots_push(&heap,&frame,values,N)==0);uint64_t calls=test_map_calls;
  for(unsigned i=0;i<N;i++) {CHECK(luxrt_object_new(&heap,&node,24,values+i)==0);uint64_t *p=luxrt_object_payload(values[i]);CHECK(p[0]==0 && p[1]==0 && p[2]==0);}
  CHECK(test_map_calls==calls);CHECK(luxrt_roots_pop(&heap,&frame)==0);CHECK(luxrt_heap_destroy(&heap)==0);CHECK(!test_mapped_bytes);
 }
 /* Invalid map and cross-heap roots fail collection without sweeping objects. */
 CHECK(luxrt_heap_init(&heap,65536,4096)==0);luxrt_descriptor bad={257,1,offsets,0,0,0};root[0]=0;CHECK(luxrt_roots_push(&heap,&frame,root,1)==0);
 CHECK(luxrt_object_new(&heap,&bad,1,root)==0);CHECK(luxrt_heap_collect(&heap)==-22);CHECK(heap.allocated_bytes==64);bad.reference_count=0;CHECK(luxrt_heap_collect(&heap)==0);
 luxrt_heap other;CHECK(luxrt_heap_init(&other,65536,4096)==0);CHECK(luxrt_value_float_bits(&other,42,root)==0);CHECK(luxrt_heap_collect(&heap)==-22);root[0]=0;CHECK(luxrt_heap_collect(&heap)==0);CHECK(luxrt_roots_pop(&heap,&frame)==0);CHECK(luxrt_heap_destroy(&heap)==0);CHECK(luxrt_heap_destroy(&other)==0);
 /* Repeated tail metadata checks length before reading any slot. */
 luxrt_descriptor repeated={258,0,0,8,0,8};CHECK(luxrt_heap_init(&heap,65536,4096)==0);root[0]=0;CHECK(luxrt_roots_push(&heap,&frame,root,1)==0);CHECK(luxrt_object_new(&heap,&repeated,24,root)==0);uint64_t *tail=luxrt_object_payload(root[0]);tail[0]=UINT64_MAX;CHECK(luxrt_heap_collect(&heap)==-22);tail[0]=2;tail[1]=LUXRT_NONE;tail[2]=LUXRT_TRUE;CHECK(luxrt_heap_collect(&heap)==0);CHECK(luxrt_roots_pop(&heap,&frame)==0);CHECK(luxrt_heap_destroy(&heap)==0);
 /* No recursive mark calls: a 30,000-node chain fits a small C stack. */
 enum{CHAIN=30000};luxrt_value *chain=calloc(CHAIN,sizeof *chain);CHECK(chain!=NULL);CHECK(luxrt_heap_init(&heap,8*1024*1024,4096)==0);CHECK(luxrt_roots_push(&heap,&frame,chain,CHAIN)==0);
 for(unsigned i=0;i<CHAIN;i++){CHECK(luxrt_object_new(&heap,&node,16,chain+i)==0);if(i)((uint64_t *)luxrt_object_payload(chain[i]))[0]=chain[i-1];}
 CHECK(luxrt_roots_pop(&heap,&frame)==0);root[0]=chain[CHAIN-1];CHECK(luxrt_roots_push(&heap,&frame,root,1)==0);CHECK(luxrt_heap_collect(&heap)==0);CHECK(heap.live_bytes==CHAIN*64UL);root[0]=0;CHECK(luxrt_heap_collect(&heap)==0);CHECK(!heap.live_bytes);CHECK(luxrt_roots_pop(&heap,&frame)==0);CHECK(luxrt_heap_destroy(&heap)==0);free(chain);
 CHECK(luxrt_heap_init(&heap,65536,4096)==0);luxrt_value out=42;test_fail_map=1;CHECK(luxrt_object_new(&heap,&node,16,&out)==-12 && out==42);test_fail_map=0;CHECK(luxrt_object_new(&heap,&node,16,&out)==0);test_fail_unmap=1;CHECK(luxrt_heap_destroy(&heap)==-5);test_fail_unmap=0;CHECK(luxrt_heap_destroy(&heap)==0);CHECK(!test_mapped_bytes);
 printf("%u managed checks passed\n",checks);return 0;
}
