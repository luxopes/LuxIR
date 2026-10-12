#define _POSIX_C_SOURCE 200112L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
uint64_t test_mapped_bytes,test_map_calls;
int test_fail_map,test_fail_unmap;
int64_t luxrt_os_map(uint64_t size){
 if(test_fail_map)return -12;
 void *p=NULL;if(posix_memalign(&p,4096,(size_t)size))return -12;
 memset(p,0,(size_t)size);test_mapped_bytes+=size;test_map_calls++;return (int64_t)(uintptr_t)p;
}
int64_t luxrt_os_unmap(void *p,uint64_t size){if(test_fail_unmap)return -5;free(p);test_mapped_bytes-=size;return 0;}
