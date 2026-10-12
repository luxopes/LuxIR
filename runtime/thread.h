#ifndef LUXRT_THREAD_H
#define LUXRT_THREAD_H
#include <stdint.h>
/* Raw Linux x86_64/arm64/rv64 threads, not a managed runtime.
 * Callback must not use libc, unwinding or managed objects. Legacy spawn has
 * no private TLS. spawn_tls requires TP from luxrt_tls_allocate_native and
 * supports statically linked local-exec TLS, including IR thread(symbol).
 * TLS mapping stays alive until join and is then caller-released.
 * Caller owns an aligned, readable/writable stack and atomic uint32 tid slot.
 * Stack and tid storage stay alive until join. One spawn per slot, no reuse
 * before join. tid initial value is zero. Guard page is caller's obligation.
 * No callback status transport; callback return exits only its own thread. */
int64_t luxrt_thread_spawn(void (*fn)(void *),void *arg,void *stack,uint64_t size,void *tid);
int64_t luxrt_thread_spawn_tls(void (*fn)(void *),void *arg,void *stack,uint64_t size,void *tid,void *tp);
int64_t luxrt_thread_join(void *tid);
#endif
