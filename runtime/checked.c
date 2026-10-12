#include "checked.h"
#include <limits.h>
int32_t luxrt_checked_add(int64_t a,int64_t b,int64_t *p){if((b>0&&a>INT64_MAX-b)||(b<0&&a<INT64_MIN-b))return -34;*p=a+b;return 0;}
int32_t luxrt_checked_sub(int64_t a,int64_t b,int64_t *p){if((b>0&&a<INT64_MIN+b)||(b<0&&a>INT64_MAX+b))return -34;*p=a-b;return 0;}
int32_t luxrt_checked_mul(int64_t a,int64_t b,int64_t *p){
 int negative=(a<0)!=(b<0);uint64_t ua=a<0?UINT64_C(0)-(uint64_t)a:(uint64_t)a,ub=b<0?UINT64_C(0)-(uint64_t)b:(uint64_t)b;
 uint64_t limit=negative?UINT64_C(9223372036854775808):INT64_MAX;if(ub&&ua>limit/ub)return -34;uint64_t value=ua*ub;
 *p=negative?(value==UINT64_C(9223372036854775808)?INT64_MIN:-(int64_t)value):(int64_t)value;return 0;
}
int32_t luxrt_checked_floordiv(int64_t a,int64_t b,int64_t *p){if(!b)return -33;if(a==INT64_MIN&&b==-1)return -34;int64_t q=a/b,r=a%b;if(r&&((a<0)!=(b<0)))q--;*p=q;return 0;}
