#ifndef LUXRT_CAPTURE_H
#define LUXRT_CAPTURE_H
#include <stdint.h>
/* Linux64, single-threaded, caller-owned spans/context. No managed allocation.
 * Request: seven 8-byte fields, 56 bytes; result: 40 bytes, offsets 0/8/16/24/28/32.
 * Buffers must not alias input/request/result or each other; capacities <= INT64_MAX.
 * timeout_ms=0 waits indefinitely, otherwise <= INT32_MAX. Deadline is checked
 * from before spawn, but synchronous exec handoff itself cannot be interrupted.
 * Caller must permit child reaping (SIGCHLD not ignored/SA_NOCLDWAIT); capture
 * temporarily blocks SIGCHLD and restores the original mask after cleanup.
 * Input uses a Unix stream socket with MSG_NOSIGNAL (not a pipe/terminal).
 * Output/error are pipes; all three directions are pumped concurrently.
 * A child that closes stdin early is allowed; input_sent reports accepted bytes.
 * 0 means communication and wait succeeded; inspect exit_code for child outcome.
 * -28 means output exceeded capacity; -110 means deadline expired. On error,
 * kill the child's process group, close owned fds and reap the direct child.
 * Descendants that explicitly leave the group are outside this guarantee.
 * Result is zeroed at entry and retains partial counts/raw wait status on failure.
 * Successful spawn sets pid; buffers contain exactly output_size/error_size bytes.
 */
struct luxrt_capture_request {
    const void *input;
    uint64_t input_size;
    void *output;
    uint64_t output_capacity;
    void *error;
    uint64_t error_capacity;
    uint64_t timeout_ms;
};
struct luxrt_capture_result {
    uint64_t input_sent, output_size, error_size;
    uint32_t raw_status;
    int32_t exit_code;
    int64_t pid;
};
uint64_t luxrt_capture_request_bytes(void);
uint64_t luxrt_capture_result_bytes(void);
int32_t luxrt_process_capture(const char *path, const char *const *argv, const char *const *envp,
                              const struct luxrt_capture_request *request,
                              struct luxrt_capture_result *result);
#endif
