#ifndef LUXRT_HEAP_HASH_INTERNAL_H
#define LUXRT_HEAP_HASH_INTERNAL_H
#include "../managed.h"
uint64_t luxrt_heap_hash_bytes(luxrt_heap *heap,const void *data,uint64_t size);
uint64_t luxrt_heap_hash_word(luxrt_heap *heap,uint64_t word);
#endif
