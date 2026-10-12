#ifndef LUXRT_THREAD_H
#define LUXRT_THREAD_H
#include <stdint.h>
/* Experimental raw Linux x86_64 threads, NOT managed/GC/TLS threads.
 * Callback must not use libc, compiler TLS, unwinding or managed objects.
 * Caller owns an aligned, readable/writable stack and atomic uint32 tid slot.
 * Stack and tid storage stay alive until join. One spawn per slot, no reuse
 * before join. tid initial value is zero. Guard page is caller's obligation.
 * No callback status transport; callback return exits only its own thread. */
int64_t luxrt_thread_spawn(void (*fn)(void *),void *arg,void *stack,uint64_t size,void *tid);
int64_t luxrt_thread_join(void *tid);
#endif
