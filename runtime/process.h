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
/* As spawn, but child starts its own process group (pgid=pid) before exec.
 * Group termination uses luxrt_os_kill(-pid, signal); descendants can explicitly
 * leave the group, so this is not a process-tree sandbox. */
int32_t luxrt_process_spawn_group(const char *path, const char *const *argv, const char *const *envp,
                                  int64_t input_fd, int64_t output_fd, int64_t error_fd, int64_t *pid);
/* Group spawn with an explicit child signal mask; NULL inherits the caller mask.
 * Parent mask restoration is unaffected. */
int32_t luxrt_process_spawn_group_mask(const char *path, const char *const *argv, const char *const *envp,
                                       int64_t input_fd, int64_t output_fd, int64_t error_fd, int64_t *pid,
                                       const uint64_t *child_mask);
/* Nonblocking exec handoff, useful for event loops and capture deadlines.
 * Start returns after fork/setup in parent; it does not promise exec succeeded.
 * Handle layout: pid Int64, error_fd Int64, bytes UInt32, record Int32 (24 bytes).
 * Handle output is unchanged on start failure. Valid handle is uniquely owned.
 * Poll returns 1 pending, 0 exec handoff, or negative setup/exec error and closes
 * error_fd on terminal status. Poll never reaps or kills; caller owns the pid.
 * While pending, error_fd can be polled for input/HUP. Caller cancelling must
 * close error_fd and terminate/reap child; group=1 requests pgid=pid, but before
 * child setup finishes group creation may still be pending. Kill pid as well.
 * Caller must prevent concurrent/automatic child reaping. */
struct luxrt_process_handoff { int64_t pid, error_fd; uint32_t bytes; int32_t record; };
uint64_t luxrt_process_handoff_bytes(void);
int32_t luxrt_process_start(const char *path, const char *const *argv, const char *const *envp,
                           int64_t input_fd, int64_t output_fd, int64_t error_fd, int32_t group,
                           const uint64_t *child_mask, struct luxrt_process_handoff *handoff);
int32_t luxrt_process_handoff_poll(struct luxrt_process_handoff *handoff);
int32_t luxrt_process_wait(int64_t pid, uint32_t *raw_status, int32_t *exit_code);
#endif
