#ifndef LUXRT_PROCESS_H
#define LUXRT_PROCESS_H
#include <stdint.h>
/* Linux raw process layer. Spawn requires a single-threaded caller and valid
 * NUL path/argv/envp arrays. envp NULL means empty environment. fd -1 inherits
 * the corresponding std descriptor, including closed/CLOEXEC state; explicit fds
 * are borrowed, and must be <= INT32_MAX. Parent descriptors stay unchanged.
 * Child retains only stdio; an internal CLOEXEC fd reports setup/exec errors.
 * Parent mask is restored after handoff; child preserves ignored signals and
 * caller mask, resets caught handlers, and masks signals during setup.
 * Close-range fallback verifies procfs and enumerates /proc/self/fd; if neither works, no exec.
 * Spawn: 0 and pid output on exec handoff, negative errno/error otherwise;
 * output unchanged on failure, failed child reaped. Killed-before-exec may
 * appear as a successful spawn followed by signalled wait, as with POSIX APIs.
 * Wait: raw wait status preserved; exit code or 128+signal, without core flag.
 * No libc or managed heap calls in the child between fork and exec.
 */
int32_t luxrt_process_spawn(const char *path, const char *const *argv, const char *const *envp,
                            int64_t input_fd, int64_t output_fd, int64_t error_fd, int64_t *pid);
int32_t luxrt_process_wait(int64_t pid, uint32_t *raw_status, int32_t *exit_code);
#endif
