#include "../../runtime/managed.h"
#include "../../runtime/os.h"
#define CHECK(c) do{if(!(c))return 1;}while(0)
static const uint64_t offsets[2]={0,8};
static const luxrt_descriptor pair={256,2,offsets,0,0,0};
int main(void) {
 luxrt_heap h;luxrt_roots frame;luxrt_value roots[4]={0,0,0,0};
 CHECK(luxrt_heap_init(&h,1048576,4096)==0);
 CHECK(luxrt_roots_push(&h,&frame,roots,4)==0);
 CHECK(luxrt_object_new(&h,&pair,16,roots)==0);
 CHECK(luxrt_object_new(&h,&pair,16,roots+1)==0);
 luxrt_value *a=luxrt_object_payload(roots[0]),*b=luxrt_object_payload(roots[1]);
 a[0]=roots[1];b[0]=roots[0];roots[1]=LUXRT_NONE;
 CHECK(luxrt_value_int(&h,INT64_MIN,roots+2)==0);
 CHECK(luxrt_value_float_bits(&h,UINT64_C(0xfff8000000001234),roots+3)==0);
 CHECK(luxrt_heap_collect(&h)==0 && h.live_bytes==256);
 int64_t integer=0;uint64_t bits=0;
 CHECK(luxrt_value_get_int(roots[2],&integer)==0 && integer==INT64_MIN);
 CHECK(luxrt_value_get_float_bits(roots[3],&bits)==0 && bits==UINT64_C(0xfff8000000001234));
 CHECK(((luxrt_value *)luxrt_object_payload(a[0]))[0]==roots[0]);
 roots[0]=0;roots[2]=0;roots[3]=0;CHECK(luxrt_heap_collect(&h)==0 && h.live_bytes==0);
 uint64_t mapped=h.mapped_bytes;
 for(uint64_t i=0;i<10000;i++) {
  CHECK(luxrt_value_int(&h,INT64_MAX,roots)==0);
  CHECK(luxrt_value_get_int(roots[0],&integer)==0 && integer==INT64_MAX);
 }
 CHECK(h.mapped_bytes==mapped);
 CHECK(luxrt_heap_destroy(&h)==-16);
 CHECK(luxrt_roots_pop(&h,&frame)==0);
 CHECK(luxrt_heap_destroy(&h)==0 && h.mapped_bytes==0);
 return 0;
}
