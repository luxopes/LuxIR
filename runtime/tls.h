#ifndef LUXRT_TLS_H
#define LUXRT_TLS_H
#include <stdint.h>
/* Static ELF TLS only. Caller-provided storage, no libc/DTV/dlopen support.
 * model 1: x86_64 variant II; 2: arm64 TCB-at-TP (16 bytes); 3: rv64 TP-at-data.
 * Templates require an aligned TLS segment, power-of-two alignment <= 1 MiB.
 * A block must outlive its thread and can be released only after join. */
struct luxrt_tls_template { const void *image; uint64_t filesz,memsz,align; };
struct luxrt_tls_block { void *mapping; uint64_t size; void *tp; };
int64_t luxrt_tls_template_from_stack(const uint64_t *stack,struct luxrt_tls_template *out);
int64_t luxrt_tls_allocate(const struct luxrt_tls_template *t,uint64_t model,struct luxrt_tls_block *out);
int64_t luxrt_tls_allocate_native(const struct luxrt_tls_template *t,struct luxrt_tls_block *out);
int64_t luxrt_tls_release(struct luxrt_tls_block *block);
int64_t luxrt_tls_set(void *tp);
void *luxrt_tls_get(void);
int64_t luxrt_tls_main(const uint64_t *stack,struct luxrt_tls_block *out);
#endif
