#ifndef LUXRT_SIGNAL_H
#define LUXRT_SIGNAL_H
#include <stdint.h>
/* Portable raw Linux descriptor: callback address (0=default,1=ignore),
 * Linux action flags, 64-bit kernel signal mask. Runtime inserts the native
 * restorer where ABI requires one; no libc sigaction struct is exposed. */
struct luxrt_signal_action {void (*handler)(int);uint64_t flags,mask;};
int64_t luxrt_signal_action(int64_t signal,const struct luxrt_signal_action *action,struct luxrt_signal_action *old);
int64_t luxrt_signal_mask(int64_t how,const uint64_t *mask,uint64_t *old);
int64_t luxrt_signal_thread(int64_t pid,int64_t tid,int64_t signal);
#endif
