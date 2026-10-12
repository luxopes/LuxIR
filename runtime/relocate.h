#ifndef LUXRT_RELOCATE_H
#define LUXRT_RELOCATE_H
#include <stdint.h>
/* Startup-only trusted ELF64 static PIE: RELA relative relocations and RELRO.
 * Must precede TLS initialization, application execution and other threads. */
int64_t luxrt_startup_relocate(const uint64_t *stack,void *base,const uint64_t *dynamic,uint64_t relative_type);
#endif
