#ifndef LUXRT_IO_H
#define LUXRT_IO_H
#include <stdint.h>
/* Retry EINTR, retain partial progress in count even on failure.
 * read_full returns 0 on complete read OR EOF: inspect count for short EOF.
 * write_full returns 0 only on complete write; zero progress -> -EIO.
 * Other raw OS errors propagate. Nonblocking EAGAIN is not busy-retried.
 * Valid buffer spans/count pointer are caller obligations. Zero size allows
 * NULL buffer, touches only count and makes no syscall. */
int64_t luxrt_read_full(int64_t fd,void *buffer,uint64_t size,uint64_t *count);
int64_t luxrt_write_full(int64_t fd,const void *buffer,uint64_t size,uint64_t *count);
#endif
