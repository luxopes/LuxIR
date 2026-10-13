#include "process.h"
#include "io.h"
#include "os.h"
#include "signal.h"
static int64_t duplicate(int64_t fd) {
    int64_t result;
    do {
        result = luxrt_os_fcntl(fd, 1030, 4);
    } while (result == -4);
    return result;
}
static int64_t move_fd(int64_t source, int64_t target, int64_t flags) {
    int64_t result;
    do {
        result = luxrt_os_dup_flags(source, target, flags);
    } while (result == -4);
    return result;
}
static int32_t close_inherited(void) {
    int64_t result;
    do {
        result = luxrt_os_close_range(4, UINT32_MAX, 0);
    } while (result == -4);
    if (result == 0)
        return 0;
    /* Old kernels and sandboxed close_range: enumerate real descriptors.
     * The scanner's own fd is excluded; malformed records fail closed. */
    int64_t directory;
    do {
        directory = luxrt_os_open("/proc/self/fd", 65536, 0);
    } while (directory == -4);
    if (directory < 0)
        return (int32_t)directory;
    uint64_t fs[16];
    do {
        result = luxrt_os_fstatfs(directory, fs);
    } while (result == -4);
    if (result < 0 || fs[0] != 0x9fa0) {
        luxrt_os_close(directory);
        return result < 0 ? (int32_t)result : -5;
    }
    unsigned char buffer[4096];
    int32_t status = 0;
    for (;;) {
        do {
            result = luxrt_os_list(directory, buffer, sizeof buffer);
        } while (result == -4);
        if (result < 0) {
            status = (int32_t)result;
            break;
        }
        if (!result)
            break;
        uint64_t at = 0;
        while (at < (uint64_t)result) {
            if ((uint64_t)result - at < 20) {
                status = -5;
                break;
            }
            uint64_t size = (uint64_t)buffer[at + 16] | (uint64_t)buffer[at + 17] << 8;
            if (size < 20 || size > (uint64_t)result - at) {
                status = -5;
                break;
            }
            if (buffer[at + 19] == '.' &&
                ((size > 20 && buffer[at + 20] == 0) ||
                 (size > 21 && buffer[at + 20] == '.' && buffer[at + 21] == 0))) {
                at += size;
                continue;
            }
            uint64_t fd = 0, n = 19;
            int32_t valid = 1;
            while (n < size && buffer[at + n]) {
                unsigned char c = buffer[at + n++];
                if (c < '0' || c > '9' || fd > (UINT64_C(2147483647) - (c - '0')) / 10) {
                    valid = 0;
                    break;
                }
                fd = fd * 10 + c - '0';
            }
            if (!valid || n == 19 || n == size) {
                status = -5;
                break;
            }
            if (valid && fd > 3 && fd != (uint64_t)directory) {
                int64_t closed = luxrt_os_close((int64_t)fd);
                /* Linux close EINTR consumes the descriptor: never retry. */
                if (closed < 0 && closed != -4 && closed != -9) {
                    status = (int32_t)closed;
                    break;
                }
            }
            at += size;
        }
        if (status)
            break;
    }
    luxrt_os_close(directory);
    return status;
}
static int32_t reset_caught_signals(void) {
    struct luxrt_signal_action old, defaults = {0, 0, 0};
    for (int64_t number = 1; number <= 64; number++) {
        int64_t result;
        do {
            result = luxrt_signal_action(number, 0, &old);
        } while (result == -4);
        if (result < 0)
            return (int32_t)result;
        if ((uintptr_t)old.handler > 1) {
            do {
                result = luxrt_signal_action(number, &defaults, 0);
            } while (result == -4);
            if (result < 0)
                return (int32_t)result;
        }
    }
    return 0;
}
static void child_failure(int64_t fd, int32_t status) {
    uint64_t count;
    luxrt_write_full(fd, &status, sizeof status, &count);
    for (;;)
        luxrt_os_exit(127);
}
int32_t luxrt_process_wait(int64_t pid, uint32_t *raw_status, int32_t *exit_code) {
    if (pid <= 0 || pid > 2147483647)
        return -22;
    uint32_t status;
    int64_t result;
    do {
        result = luxrt_os_wait(pid, &status, 0);
    } while (result == -4);
    if (result < 0)
        return (int32_t)result;
    uint32_t signal = status & 127;
    if (result != pid || signal == 127)
        return -5;
    *raw_status = status;
    *exit_code = signal ? (int32_t)(128 + signal) : (int32_t)((status >> 8) & 255);
    return 0;
}
static int32_t spawn_impl(const char *path, const char *const *argv, const char *const *envp,
                          int64_t input_fd, int64_t output_fd, int64_t error_fd, int64_t *outpid,
                          int32_t group, const uint64_t *child_mask,
                          struct luxrt_process_handoff *handoff) {
    if (!path || !argv || input_fd < -1 || output_fd < -1 || error_fd < -1 ||
        input_fd > 2147483647 || output_fd > 2147483647 || error_fd > 2147483647)
        return -22;
    int64_t requested[3] = {input_fd, output_fd, error_fd};
    int64_t saved[3] = {-1, -1, -1}, inherit_flags[3] = {0, 0, 0};
    int32_t pipefd[2] = {-1, -1}, status = 0, masked = 0;
    uint64_t all = UINT64_MAX, oldmask = 0;
    int64_t success_pid = -1, result_fd = -1;
    for (int32_t i = 0; i < 3; i++) {
        int64_t source = requested[i] == -1 ? i : requested[i];
        if (requested[i] == -1) {
            int64_t flags;
            do {
                flags = luxrt_os_fcntl(source, 1, 0);
            } while (flags == -4);
            if (flags == -9)
                continue;
            if (flags < 0) {
                status = (int32_t)flags;
                goto cleanup;
            }
            inherit_flags[i] = (flags & 1) ? 524288 : 0;
        }
        saved[i] = duplicate(source);
        if (saved[i] < 0) {
            if (saved[i] == -9 && requested[i] == -1) {
                saved[i] = -1;
                continue;
            }
            status = (int32_t)saved[i];
            goto cleanup;
        }
    }
    int64_t result;
    do {
        result = luxrt_os_pipe(pipefd, handoff ? 2048 : 0);
    } while (result == -4);
    if (result < 0) {
        status = (int32_t)result;
        goto cleanup;
    }
    do {
        result = luxrt_signal_mask(2, &all, &oldmask);
    } while (result == -4);
    if (result < 0) {
        status = (int32_t)result;
        goto cleanup;
    }
    masked = 1;
    int64_t pid;
    do {
        pid = luxrt_os_fork();
    } while (pid == -4);
    if (pid < 0) {
        status = (int32_t)pid;
        goto cleanup;
    }
    if (!pid) {
        if (group) {
            do {
                result = luxrt_os_setpgid(0, 0);
            } while (result == -4);
            if (result < 0)
                child_failure(pipefd[1], (int32_t)result);
        }
        /* Move the error channel first: its old fd can be 0, 1 or 2. */
        if (pipefd[1] != 3) {
            result = move_fd(pipefd[1], 3, 524288);
            if (result < 0)
                child_failure(pipefd[1], (int32_t)result);
        }
        for (int32_t i = 0; i < 3; i++) {
            if (saved[i] < 0)
                luxrt_os_close(i);
            else {
                result = move_fd(saved[i], i, inherit_flags[i]);
                if (result < 0)
                    child_failure(3, (int32_t)result);
            }
        }
        status = close_inherited();
        if (status)
            child_failure(3, status);
        status = reset_caught_signals();
        if (status)
            child_failure(3, status);
        do {
            result = luxrt_signal_mask(2, child_mask ? child_mask : &oldmask, 0);
        } while (result == -4);
        if (result < 0)
            child_failure(3, (int32_t)result);
        do {
            result = luxrt_os_exec(path, (void *)argv, (void *)envp);
        } while (result == -4);
        child_failure(3, result < 0 ? (int32_t)result : -5);
    }
    luxrt_os_close(pipefd[1]);
    pipefd[1] = -1;
    for (int32_t i = 0; i < 3; i++)
        if (saved[i] >= 0) {
            luxrt_os_close(saved[i]);
            saved[i] = -1;
        }
    if (handoff) {
        result_fd = pipefd[0];
        pipefd[0] = -1;
        success_pid = pid;
        goto cleanup;
    }
    int32_t exec_status = 0;
    uint64_t bytes;
    result = luxrt_read_full(pipefd[0], &exec_status, sizeof exec_status, &bytes);
    luxrt_os_close(pipefd[0]);
    pipefd[0] = -1;
    if (result < 0 || bytes) {
        status = result < 0                                       ? (int32_t)result
                 : bytes == sizeof exec_status && exec_status < 0 ? exec_status
                                                                  : -5;
        /* On a broken handshake, terminate our child before reaping it. */
        if (result < 0 || bytes != sizeof exec_status || exec_status >= 0)
            luxrt_os_kill(pid, 9);
        uint32_t raw;
        int32_t exit_code;
        luxrt_process_wait(pid, &raw, &exit_code);
        goto cleanup;
    }
    success_pid = pid;
cleanup:
    for (int32_t i = 0; i < 3; i++)
        if (saved[i] >= 0)
            luxrt_os_close(saved[i]);
    for (int32_t i = 0; i < 2; i++)
        if (pipefd[i] >= 0)
            luxrt_os_close(pipefd[i]);
    if (masked) {
        int64_t restored;
        do {
            restored = luxrt_signal_mask(2, &oldmask, 0);
        } while (restored == -4);
        if (restored < 0 && !status) {
            status = (int32_t)restored;
            if (success_pid > 0) {
                luxrt_os_kill(success_pid, 9);
                uint32_t raw;
                int32_t code;
                luxrt_process_wait(success_pid, &raw, &code);
                success_pid = -1;
            }
        }
    }
    if (status && result_fd >= 0)
        luxrt_os_close(result_fd);
    if (!status) {
        *outpid = success_pid;
        if (handoff) {
            handoff->pid = success_pid;
            handoff->error_fd = result_fd;
            handoff->bytes = 0;
            handoff->record = 0;
        }
    }
    return status;
}

int32_t luxrt_process_spawn(const char *path, const char *const *argv, const char *const *envp,
                            int64_t input, int64_t output, int64_t error, int64_t *pid) {
    return spawn_impl(path, argv, envp, input, output, error, pid, 0, 0, 0);
}
int32_t luxrt_process_spawn_group(const char *path, const char *const *argv,
                                  const char *const *envp, int64_t input, int64_t output,
                                  int64_t error, int64_t *pid) {
    return spawn_impl(path, argv, envp, input, output, error, pid, 1, 0, 0);
}

int32_t luxrt_process_spawn_group_mask(const char *path, const char *const *argv,
                                       const char *const *envp, int64_t input, int64_t output,
                                       int64_t error, int64_t *pid, const uint64_t *child_mask) {
    return spawn_impl(path, argv, envp, input, output, error, pid, 1, child_mask, 0);
}

uint64_t luxrt_process_handoff_bytes(void) { return sizeof(struct luxrt_process_handoff); }
int32_t luxrt_process_start(const char *path, const char *const *argv, const char *const *envp,
                            int64_t input, int64_t output, int64_t error, int32_t group,
                            const uint64_t *mask, struct luxrt_process_handoff *handoff) {
    if (group != 0 && group != 1)
        return -22;
    int64_t pid;
    return spawn_impl(path, argv, envp, input, output, error, &pid, group, mask, handoff);
}
int32_t luxrt_process_handoff_poll(struct luxrt_process_handoff *h) {
    if (h->pid <= 0 || h->error_fd < 0 || h->error_fd > INT32_MAX || h->bytes > 3)
        return -22;
    int64_t n = luxrt_os_read(h->error_fd, (unsigned char *)&h->record + h->bytes, 4 - h->bytes);
    if (n == -4 || n == -11)
        return 1;
    int32_t status;
    if (n < 0)
        status = (int32_t)n;
    else if (n == 0)
        status = h->bytes ? -5 : 0;
    else {
        h->bytes += (uint32_t)n;
        if (h->bytes < 4)
            return 1;
        status = h->record < 0 ? h->record : -5;
    }
    luxrt_os_close(h->error_fd);
    h->error_fd = -1;
    return status;
}
