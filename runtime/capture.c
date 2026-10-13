#include "capture.h"
#include "os.h"
#include "process.h"
#include "signal.h"
struct poll_fd {
    int32_t fd;
    int16_t events, revents;
};
static int32_t now(uint64_t *out) {
    int64_t ts[2], status;
    do {
        status = luxrt_os_clock(1, ts);
    } while (status == -4);
    if (status < 0)
        return (int32_t)status;
    if (ts[0] < 0 || (uint64_t)ts[0] > (UINT64_MAX - UINT64_C(999999999)) / UINT64_C(1000000000) ||
        ts[1] < 0 || ts[1] >= 1000000000)
        return -75;
    *out = (uint64_t)ts[0] * UINT64_C(1000000000) + (uint64_t)ts[1];
    return 0;
}
static void close_fd(int32_t *fd) {
    if (*fd >= 0) {
        luxrt_os_close(*fd);
        *fd = -1;
    }
}
static int32_t nonblocking(int32_t fd) {
    int64_t flags, status;
    do {
        flags = luxrt_os_fcntl(fd, 3, 0);
    } while (flags == -4);
    if (flags < 0)
        return (int32_t)flags;
    do {
        status = luxrt_os_fcntl(fd, 4, flags | 2048);
    } while (status == -4);
    return status < 0 ? (int32_t)status : 0;
}
static int32_t drain(int32_t *fd, void *buffer, uint64_t capacity, uint64_t *size,
                     int32_t *progress) {
    if (*fd < 0)
        return 0;
    unsigned char extra;
    uint64_t left = capacity - *size;
    if (left > 8192)
        left = 8192;
    void *destination = left ? (unsigned char *)buffer + *size : &extra;
    int64_t n = luxrt_os_read(*fd, destination, left ? left : 1);
    if (n == -4 || n == -11)
        return 0;
    if (n < 0)
        return (int32_t)n;
    if (!n) {
        close_fd(fd);
        *progress = 1;
        return 0;
    }
    if (!left)
        return -28;
    *size += (uint64_t)n;
    *progress = 1;
    return 0;
}
uint64_t luxrt_capture_request_bytes(void) { return sizeof(struct luxrt_capture_request); }
uint64_t luxrt_capture_result_bytes(void) { return sizeof(struct luxrt_capture_result); }
int32_t luxrt_process_capture(const char *path, const char *const *argv, const char *const *envp,
                              const struct luxrt_capture_request *q,
                              struct luxrt_capture_result *r) {
    r->input_sent = 0;
    r->output_size = 0;
    r->error_size = 0;
    r->raw_status = 0;
    r->exit_code = 0;
    r->pid = 0;
    if (!path || !argv || (!q->input && q->input_size) || (!q->output && q->output_capacity) ||
        (!q->error && q->error_capacity) || q->input_size > INT64_MAX ||
        q->output_capacity > INT64_MAX || q->error_capacity > INT64_MAX ||
        q->timeout_ms > INT32_MAX)
        return -22;
    int32_t in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    int32_t status = 0, reaped = 0, masked = 0, pending = 1;
    struct luxrt_process_handoff handoff = {0, -1, 0, 0};
    uint64_t oldmask = 0, childmask = UINT64_C(1) << 16;
    int64_t n;
    struct luxrt_signal_action childaction;
    do {
        n = luxrt_signal_action(17, 0, &childaction);
    } while (n == -4);
    if (n < 0)
        return (int32_t)n;
    if ((uintptr_t)childaction.handler == 1 || (childaction.flags & 2))
        return -22;
    do {
        n = luxrt_signal_mask(0, &childmask, &oldmask);
    } while (n == -4);
    if (n < 0)
        return (int32_t)n;
    masked = 1;
    uint64_t start = 0;
    if (q->timeout_ms) {
        status = now(&start);
        if (status)
            goto cleanup;
    }
    do {
        n = luxrt_os_socketpair(1, 1, 0, in);
    } while (n == -4);
    if (n < 0) {
        status = (int32_t)n;
        goto cleanup;
    }
    do {
        n = luxrt_os_pipe(out, 0);
    } while (n == -4);
    if (n < 0) {
        status = (int32_t)n;
        goto cleanup;
    }
    do {
        n = luxrt_os_pipe(err, 0);
    } while (n == -4);
    if (n < 0) {
        status = (int32_t)n;
        goto cleanup;
    }
    status = nonblocking(in[0]);
    if (status)
        goto cleanup;
    status = nonblocking(out[0]);
    if (status)
        goto cleanup;
    status = nonblocking(err[0]);
    if (status)
        goto cleanup;
    status = luxrt_process_start(path, argv, envp, in[1], out[1], err[1], 1, &oldmask, &handoff);
    if (status)
        goto cleanup;
    r->pid = handoff.pid;
    close_fd(&in[1]);
    close_fd(&out[1]);
    close_fd(&err[1]);
    for (;;) {
        uint64_t delay = UINT64_C(50000000);
        if (q->timeout_ms) {
            uint64_t current;
            status = now(&current);
            if (status)
                break;
            uint64_t limit = q->timeout_ms * UINT64_C(1000000);
            if (current < start) {
                status = -75;
                break;
            }
            if (current - start >= limit) {
                status = -110;
                break;
            }
            uint64_t remaining = limit - (current - start);
            if (remaining < delay)
                delay = remaining;
        }
        int32_t progress = 0;
        if (pending) {
            int32_t handoff_status = luxrt_process_handoff_poll(&handoff);
            if (handoff_status < 0) {
                status = handoff_status;
                break;
            }
            if (handoff_status == 0) {
                pending = 0;
                progress = 1;
            }
        }
        status = drain(&out[0], q->output, q->output_capacity, &r->output_size, &progress);
        if (status)
            break;
        status = drain(&err[0], q->error, q->error_capacity, &r->error_size, &progress);
        if (status)
            break;
        if (in[0] >= 0) {
            if (r->input_sent == q->input_size) {
                close_fd(&in[0]);
                progress = 1;
            } else {
                uint64_t size = q->input_size - r->input_sent;
                if (size > 8192)
                    size = 8192;
                n = luxrt_os_send(in[0], (unsigned char *)q->input + r->input_sent, size, 0, 0, 0);
                if (n > 0) {
                    r->input_sent += (uint64_t)n;
                    progress = 1;
                } else if (n == -32 || n == -104) {
                    close_fd(&in[0]);
                    progress = 1;
                } else if (n != -4 && n != -11) {
                    status = n < 0 ? (int32_t)n : -5;
                    break;
                }
            }
        }
        /* Retain the leader's pid until all streams close: avoid a reused pgid
         * when descendants keep a pipe open and group termination is needed. */
        if (!pending && in[0] < 0 && out[0] < 0 && err[0] < 0) {
            uint32_t raw;
            n = luxrt_os_wait(r->pid, &raw, 1);
            if (n == r->pid) {
                r->raw_status = raw;
                r->exit_code =
                    (raw & 127) ? 128 + (int32_t)(raw & 127) : (int32_t)((raw >> 8) & 255);
                reaped = 1;
                break;
            }
            if (n < 0 && n != -4) {
                status = (int32_t)n;
                break;
            }
            if (n > 0) {
                status = -5;
                break;
            }
        }
        if (progress)
            continue;
        struct poll_fd fds[4] = {
            {in[0], 4, 0}, {out[0], 1, 0}, {err[0], 1, 0}, {(int32_t)handoff.error_fd, 1, 0}};
        int64_t ts[2] = {0, (int64_t)delay};
        n = luxrt_os_poll(fds, 4, ts);
        if (n < 0 && n != -4) {
            status = (int32_t)n;
            break;
        }
    }
cleanup:
    if (r->pid > 0 && !reaped && status) {
        luxrt_os_kill(-r->pid, 9);
        luxrt_os_kill(r->pid, 9); /* Group creation can still be pending. */
    }
    if (handoff.error_fd >= 0)
        luxrt_os_close(handoff.error_fd);
    close_fd(&in[0]);
    close_fd(&in[1]);
    close_fd(&out[0]);
    close_fd(&out[1]);
    close_fd(&err[0]);
    close_fd(&err[1]);
    if (r->pid > 0 && !reaped) {
        int32_t wait_status = luxrt_process_wait(r->pid, &r->raw_status, &r->exit_code);
        if (!status)
            status = wait_status;
    }
    if (masked) {
        do {
            n = luxrt_signal_mask(2, &oldmask, 0);
        } while (n == -4);
        if (n < 0 && !status)
            status = (int32_t)n;
    }
    return status;
}
