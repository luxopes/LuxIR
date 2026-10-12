#ifndef LUXRT_CONTEXT_H
#define LUXRT_CONTEXT_H
#include <stdint.h>
/* Snapshot callee-saved registers at the call boundary, then caller SP.
 * 7 words x86_64 (rbx,rbp,r12..r15,SP), 20 arm64 (x19..x29,d8..d15,SP),
 * 25 rv64 (s0..s11,fs0..fs11,SP). Always reserve 25 aligned uint64 words.
 * Floating registers are saved as raw bits. Not setjmp/longjmp, no unwinding.
 * Caller roots in caller-saved registers must be spilled by ordinary call ABI.
 * Scan snapshot AND the live stack interval; stop-the-world is caller policy. */
uint64_t luxrt_context_words(void);
void *luxrt_capture_regs(uint64_t *snapshot);
void *luxrt_startup_stack_top(const uint64_t *kernel_stack);
#endif
