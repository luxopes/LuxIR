# lsl: internal-abi
# Shared-memory thread runtime: futex mutexes, clone-based spawn, thread table,
# stop-the-world gc() support and join. Pure syscalls only.
#
# State block (RBX) words used by threads:
#   +88  native-library mapping mutex
#   +96  heap mutex word (runtime_alloc / gc() serialization)
#  +104  thread-table mutex word
#  +112  thread table pointer (two anonymous mmap pages, 8192 bytes)
#  +120  main thread stack top (gc() identity check against r15)
#  +128  gc gate word: 1 while the main thread collects, 0 otherwise
#  +136  threads-started flag: set by the first thread_spawn, never cleared
#
# Thread table: [+0 reserved], then 64-byte entries:
#   [+0 kernel tid, +8 stack base (top of the 8 MiB mapping),
#    +16 status, +24 saved rsp of a suspended worker,
#    +32.. the worker's thread-local block (fs base): +32 pending error,
#    +40 error flag, +48 stack top, +56 runtime state pointer]
# The top bit of the tid word marks a slot whose exited thread has already
# been joined. Only such a slot (or one that never held a thread) is reused,
# so an exited but unjoined thread keeps its record and join still finds it.
# status: 0 = free/exited (cleared by the kernel via CLONE_CHILD_CLEARTID),
# 1 = running, 2 = suspended in the gc handler, 3 = slot reserved by a spawn
# in progress, 4 = signalled by gc() but not yet suspended.
# A worker's stack base is published before its status becomes 1, so gc()
# never sees a live entry with an invalid stack range.
#
# Mutex protocol (0 free, 1 locked, 2 locked with waiters): uncontended
# lock/unlock are single lock-prefixed instructions without syscalls, so
# threaded programs pay two atomic ops per allocation and single-threaded
# programs only a flag load (see the guarded heap mutex below).

load runtime_abi_linux_x86_64 as abi_x86_64
load _platform_linux_x86_64 as platform
load assembler_x86_64 as assembler
load x86_64

function emit_mutex_pair(output, lock_label, wait_label, unlock_label, offset, guarded):
    # Clobbers only caller-saved registers; RBX must point at the state block.
    # A guarded mutex is a no-op until the first thread_spawn sets state+136,
    # so single-threaded programs pay nothing on the allocation path.
    call assembler.label(output, lock_label)
    if guarded:
        call assembler.emit(output, x86_64.load_memory(0, 3, 136))
        call assembler.emit(output, x86_64.test_reg_reg(0, 0))
        call assembler.relative(output, [15, 132], "thread_ret")
    end
    call assembler.emit(output, x86_64.xor_reg_reg(0, 0))
    call assembler.emit(output, x86_64.mov_imm64(1, 1))
    call assembler.emit(output, x86_64.lock_cmpxchg_memory(3, offset, 1))
    call assembler.relative(output, [15, 133], wait_label)
    call assembler.emit(output, x86_64.ret())
    call assembler.label(output, wait_label)
    # Mark the word contended; if it was free we now own it.
    call assembler.emit(output, x86_64.mov_imm64(0, 2))
    call assembler.emit(output, x86_64.lock_xchg_memory(3, offset, 0))
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 132], "thread_ret")
    # futex_wait(&word, 2); spurious wakeups and EINTR retry the xchg.
    call assembler.emit(output, x86_64.lea_base(7, 3, offset))
    call assembler.emit(output, x86_64.xor_reg_reg(6, 6))
    call assembler.emit(output, x86_64.mov_imm64(2, 2))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_FUTEX))
    call assembler.emit(output, x86_64.xor_reg_reg(10, 10))
    call assembler.emit(output, x86_64.xor_reg_reg(8, 8))
    call assembler.emit(output, x86_64.syscall())
    call assembler.relative(output, [233], wait_label)
    call assembler.label(output, unlock_label)
    if guarded:
        call assembler.emit(output, x86_64.load_memory(0, 3, 136))
        call assembler.emit(output, x86_64.test_reg_reg(0, 0))
        call assembler.relative(output, [15, 132], "thread_ret")
    end
    call assembler.emit(output, x86_64.xor_reg_reg(0, 0))
    call assembler.emit(output, x86_64.lock_xchg_memory(3, offset, 0))
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 2))
    call assembler.relative(output, [15, 133], "thread_ret")
    # futex_wake(&word, 1)
    call assembler.emit(output, x86_64.lea_base(7, 3, offset))
    call assembler.emit(output, x86_64.mov_imm64(6, 1))
    call assembler.emit(output, x86_64.mov_imm64(2, 1))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_FUTEX))
    call assembler.emit(output, x86_64.xor_reg_reg(10, 10))
    call assembler.emit(output, x86_64.xor_reg_reg(8, 8))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.ret())
end

function emit_mutex(output):
    call assembler.label(output, "thread_ret")
    call assembler.emit(output, x86_64.ret())
    call emit_mutex_pair(output, "runtime_native_lock", "native_mutex_wait", "runtime_native_unlock", 88, False)
    call emit_mutex_pair(output, "runtime_mutex_lock", "mutex_wait", "runtime_mutex_unlock", 96, True)
    call emit_mutex_pair(output, "runtime_table_lock", "table_wait", "runtime_table_unlock", 104, False)
end

function emit_entry_lookup(output, found_label, missing_label):
    # rax = kernel tid -> r9 = entry with that tid, else jump to missing_label.
    # The reaped bit is masked off, so a joined slot is still found by tid.
    # Tid 0 belongs to no thread; free and reserved slots hold it.
    call assembler.emit(output, x86_64.test_reg_reg(0, 0))
    call assembler.relative(output, [15, 132], missing_label)
    call assembler.emit(output, x86_64.load_memory(8, 3, 112))
    call assembler.emit(output, x86_64.mov_imm64(11, 9223372036854775807))
    call assembler.emit(output, x86_64.xor_reg_reg(1, 1))
    call assembler.label(output, found_label + "_scan")
    call assembler.emit(output, x86_64.cmp_reg_imm32(1, 127))
    call assembler.relative(output, [15, 141], missing_label)
    call assembler.emit(output, x86_64.move_reg_reg(10, 1))
    call assembler.emit(output, x86_64.shift_left_imm(10, 6))
    call assembler.emit(output, x86_64.lea_indexed(9, 8, 10, 0))
    call assembler.emit(output, x86_64.add_reg_imm8(9, 8))
    call assembler.emit(output, x86_64.load_memory(2, 9, 0))
    call assembler.emit(output, x86_64.and_reg_reg(2, 11))
    call assembler.emit(output, x86_64.cmp_reg_reg(2, 0))
    call assembler.relative(output, [15, 132], found_label)
    call assembler.emit(output, x86_64.increment(1))
    call assembler.relative(output, [233], found_label + "_scan")
    call assembler.label(output, found_label)
end

function emit_slot_address(output):
    # rcx = slot index, r8 = table -> r9 = that entry.
    call assembler.emit(output, x86_64.move_reg_reg(10, 1))
    call assembler.emit(output, x86_64.shift_left_imm(10, 6))
    call assembler.emit(output, x86_64.lea_indexed(9, 8, 10, 0))
    call assembler.emit(output, x86_64.add_reg_imm8(9, 8))
end

function emit_spawn(output):
    # runtime_thread_spawn: rdi = tagged Function -> kernel tid in rax, or
    # -1 when the table is full or the stack mapping failed. The table lock
    # is held across clone so the kernel tid is published before anyone can
    # observe the entry; the child takes no lock before the parent releases.
    call assembler.label(output, "runtime_thread_spawn")
    call assembler.relative(output, [232], "runtime_stdio_direct")
    call assembler.emit(output, x86_64.function_prologue(64))
    call assembler.emit(output, x86_64.store_stack(-8, 7))
    # Public callers must supply a live tagged Function.  Treating an arbitrary
    # scalar as a raw code pointer would let thread_spawn(0) terminate the whole
    # process because clone workers share the signal group.
    call assembler.relative(output, [232], "runtime_is_heap_object")
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 132], "spawn_invalid_function")
    call assembler.emit(output, x86_64.load_stack(7, -8))
    call assembler.emit(output, x86_64.load_memory(0, 7, 0))
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 10))
    call assembler.relative(output, [15, 133], "spawn_invalid_function")
    call assembler.emit(output, x86_64.load_memory(0, 7, 8))
    call assembler.emit(output, x86_64.test_reg_reg(0, 0))
    call assembler.relative(output, [15, 132], "spawn_invalid_function")
    # Enable heap locking before a second thread can exist.
    call assembler.emit(output, x86_64.store_memory_imm32(3, 136, 1))
    call assembler.relative(output, [232], "runtime_table_lock")
    call assembler.emit(output, x86_64.load_memory(8, 3, 112))
    call assembler.emit(output, x86_64.xor_reg_reg(1, 1))
    # First pass: a slot that never held a thread, or one whose thread exited
    # and was already joined. Records of unjoined threads stay untouched.
    call assembler.label(output, "spawn_find_slot")
    call assembler.emit(output, x86_64.cmp_reg_imm32(1, 127))
    call assembler.relative(output, [15, 141], "spawn_find_unjoined")
    call emit_slot_address(output)
    call assembler.emit(output, x86_64.load_memory(2, 9, 16))
    call assembler.emit(output, x86_64.cmp_reg_imm8(2, 0))
    call assembler.relative(output, [15, 133], "spawn_next_slot")
    call assembler.emit(output, x86_64.load_memory(2, 9, 0))
    call assembler.emit(output, x86_64.test_reg_reg(2, 2))
    call assembler.relative(output, [15, 132], "spawn_slot_ready")
    call assembler.emit(output, x86_64.mov_imm64(11, -9223372036854775808))
    call assembler.emit(output, x86_64.and_reg_reg(2, 11))
    call assembler.emit(output, x86_64.test_reg_reg(2, 2))
    call assembler.relative(output, [15, 133], "spawn_slot_ready")
    call assembler.label(output, "spawn_next_slot")
    call assembler.emit(output, x86_64.increment(1))
    call assembler.relative(output, [233], "spawn_find_slot")
    # Second pass: the table is otherwise full, so recycle any exited slot.
    # A join for that forgotten tid reports 126 exactly as it did before.
    call assembler.label(output, "spawn_find_unjoined")
    call assembler.emit(output, x86_64.xor_reg_reg(1, 1))
    call assembler.label(output, "spawn_find_unjoined_scan")
    call assembler.emit(output, x86_64.cmp_reg_imm32(1, 127))
    call assembler.relative(output, [15, 141], "spawn_no_slot")
    call emit_slot_address(output)
    call assembler.emit(output, x86_64.load_memory(2, 9, 16))
    call assembler.emit(output, x86_64.cmp_reg_imm8(2, 0))
    call assembler.relative(output, [15, 132], "spawn_slot_ready")
    call assembler.emit(output, x86_64.increment(1))
    call assembler.relative(output, [233], "spawn_find_unjoined_scan")
    call assembler.label(output, "spawn_slot_ready")
    # r9 = entry. Claim it (status 3 = reserved: never signalled or scanned)
    # and reuse the previous occupant's stack when the slot has one; the
    # kernel cleared its status only after that thread fully exited.
    call assembler.emit(output, x86_64.store_memory_imm32(9, 16, 3))
    call assembler.emit(output, x86_64.store_memory_imm32(9, 0, 0))
    call assembler.emit(output, x86_64.store_stack(-16, 9))
    call assembler.emit(output, x86_64.load_memory(10, 9, 8))
    call assembler.emit(output, x86_64.store_stack(-24, 10))
    call assembler.relative(output, [232], "runtime_table_unlock")
    call assembler.emit(output, x86_64.load_stack(10, -24))
    call assembler.emit(output, x86_64.cmp_reg_imm8(10, 0))
    call assembler.relative(output, [15, 133], "spawn_stack_ready")
    # mmap a fresh 8 MiB stack plus a bottom guard page
    # (MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK).
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_MMAP))
    call assembler.emit(output, x86_64.xor_reg_reg(7, 7))
    call assembler.emit(output, x86_64.mov_imm64(6, 8392704))
    call assembler.emit(output, x86_64.mov_imm64(2, 3))
    call assembler.emit(output, x86_64.mov_imm64(10, 131106))
    call assembler.emit(output, x86_64.mov_imm64(8, -1))
    call assembler.emit(output, x86_64.xor_reg_reg(9, 9))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 140], "spawn_stack_failed")
    call assembler.emit(output, x86_64.store_stack(-32, 0))
    call assembler.emit(output, x86_64.move_reg_reg(7, 0))
    call assembler.emit(output, x86_64.mov_imm64(6, 4096))
    call assembler.emit(output, x86_64.xor_reg_reg(2, 2))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_MPROTECT))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 133], "spawn_stack_guard_failed")
    call assembler.emit(output, x86_64.load_stack(0, -32))
    call assembler.emit(output, x86_64.add_reg_imm32(0, 8392704))
    call assembler.emit(output, x86_64.store_stack(-24, 0))
    call assembler.relative(output, [233], "spawn_stack_ready")
    call assembler.label(output, "spawn_stack_guard_failed")
    call assembler.emit(output, x86_64.load_stack(7, -32))
    call assembler.emit(output, x86_64.mov_imm64(6, 8392704))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_MUNMAP))
    call assembler.emit(output, x86_64.syscall())
    call assembler.relative(output, [233], "spawn_stack_failed")
    call assembler.label(output, "spawn_stack_ready")
    # The child finds its entry and Function at the top of its stack.
    call assembler.emit(output, x86_64.load_stack(10, -24))
    call assembler.emit(output, x86_64.load_stack(9, -16))
    call assembler.emit(output, x86_64.store_memory(10, -16, 9))
    call assembler.emit(output, x86_64.load_stack(7, -8))
    call assembler.emit(output, x86_64.store_memory(10, -8, 7))
    # Publish stack_base and status under the table mutex so a concurrent
    # gc() never observes status=1 with an invalid stack range.
    call assembler.relative(output, [232], "runtime_table_lock")
    call assembler.emit(output, x86_64.load_stack(9, -16))
    call assembler.emit(output, x86_64.load_stack(10, -24))
    call assembler.emit(output, x86_64.store_memory(9, 8, 10))
    call assembler.emit(output, x86_64.store_memory_imm32(9, 16, 1))
    # clone(flags, child_stack = top - 16, parent_tid = 0,
    #       child_tid = &entry.status, tls = 0) with CLONE_VM|CLONE_FS|
    # CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|
    # CLONE_CHILD_CLEARTID: the kernel zeroes the status word and wakes its
    # futex only once the thread has fully exited, so joiners and stack
    # reuse never race with a thread still running on that stack.
    # The entry's second half (entry+32..) is the child's thread-local block:
    # +32 pending error, +40 error flag, +48 stack top, +56 runtime state;
    # CLONE_SETTLS points
    # fs at it.
    call assembler.emit(output, x86_64.store_memory_imm32(9, 32, 0))
    call assembler.emit(output, x86_64.store_memory_imm32(9, 40, 0))
    call assembler.emit(output, x86_64.store_memory(9, 48, 10))
    # A signal can interrupt QBE code while RBX contains an ordinary local.
    # Publish the runtime state independently before CLONE_SETTLS makes this
    # worker TLS block visible, so its GC handler never relies on that RBX.
    call assembler.emit(output, x86_64.store_memory(9, 56, 3))
    call assembler.emit(output, x86_64.lea_base(6, 10, -16))
    call assembler.emit(output, x86_64.lea_base(8, 9, 32))
    call assembler.emit(output, x86_64.lea_base(10, 9, 16))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_CLONE))
    call assembler.emit(output, x86_64.mov_imm64(7, platform.THREAD_CLONE_FLAGS))
    call assembler.emit(output, x86_64.xor_reg_reg(2, 2))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 132], "runtime_thread_entry")
    call assembler.relative(output, [15, 140], "spawn_clone_failed")
    # Parent: publish the kernel tid and return it.
    call assembler.emit(output, x86_64.store_memory(9, 0, 0))
    call assembler.emit(output, x86_64.store_stack(-40, 0))
    call assembler.relative(output, [232], "runtime_table_unlock")
    call assembler.emit(output, x86_64.load_stack(0, -40))
    call assembler.emit(output, x86_64.function_epilogue())
    call assembler.label(output, "spawn_clone_failed")
    # Free the slot again; the stack mapping stays for the next occupant.
    call assembler.emit(output, x86_64.store_memory_imm32(9, 16, 0))
    call assembler.relative(output, [232], "runtime_table_unlock")
    call assembler.emit(output, x86_64.mov_imm64(0, -1))
    call assembler.emit(output, x86_64.function_epilogue())
    call assembler.label(output, "spawn_stack_failed")
    call assembler.relative(output, [232], "runtime_table_lock")
    call assembler.emit(output, x86_64.load_stack(9, -16))
    call assembler.emit(output, x86_64.store_memory_imm32(9, 16, 0))
    call assembler.label(output, "spawn_no_slot")
    call assembler.relative(output, [232], "runtime_table_unlock")
    call assembler.emit(output, x86_64.mov_imm64(0, -1))
    call assembler.emit(output, x86_64.function_epilogue())
    call assembler.label(output, "spawn_invalid_function")
    call assembler.emit(output, x86_64.mov_imm64(0, -1))
    call assembler.emit(output, x86_64.function_epilogue())
end

function emit_stderr_literal(output, label, message):
    # Zápis ASCII literálu na fd 2; data leží přímo v kódu za skokem.
    call assembler.relative(output, x86_64.lea_rip_prefix(6), label)
    call assembler.emit(output, x86_64.mov_imm64(2, len(message)))
    call assembler.emit(output, x86_64.mov_imm64(7, 2))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_WRITE))
    call assembler.emit(output, x86_64.syscall())
    call assembler.relative(output, [233], label + "_end")
    call assembler.label(output, label)
    data = []
    for character in message:
        data.append(ord(character))
    end
    call assembler.emit(output, data)
    call assembler.label(output, label + "_end")
end

function emit_entry(output):
    # Child start: rsp = stack top - 16 with [rsp] = entry, [rsp+8] = tagged
    # Function. rbx (state block) is inherited from the spawning thread; fs points
    # at the entry's thread-local block.
    call assembler.label(output, "runtime_thread_entry")
    call assembler.emit(output, x86_64.load_memory(7, 4, 8))
    call assembler.emit(output, x86_64.push(5))
    call assembler.emit(output, x86_64.move_reg_reg(5, 4))
    call assembler.emit(output, x86_64.sub_reg_imm(4, 24))
    call assembler.relative(output, [232], "runtime_unbox_dynamic_function")
    call assembler.emit(output, x86_64.move_reg_reg(11, 0))
    call assembler.emit(output, x86_64.call_reg(11))
    # Worker skončil nezachycenou chybou: vypsat ji na stderr, jinak by vlákno
    # zaniklo tiše. Zpráva je String v TLS (+8 délka, +16 bajty).
    call assembler.emit(output, x86_64.cmp_fs_imm8(abi_x86_64.TLS_ERROR_FLAG, 0))
    call assembler.relative(output, [15, 132], "thread_entry_exit")
    call emit_stderr_literal(output, "thread_worker_prefix", "LSL worker error: ")
    call assembler.emit(output, x86_64.load_fs(6, abi_x86_64.TLS_ERROR))
    call assembler.emit(output, x86_64.load_memory(2, 6, 8))
    call assembler.emit(output, x86_64.lea_base(6, 6, 16))
    call assembler.emit(output, x86_64.mov_imm64(7, 2))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_WRITE))
    call assembler.emit(output, x86_64.syscall())
    call emit_stderr_literal(output, "thread_worker_newline", chr(10))
    call assembler.label(output, "thread_entry_exit")
    # Thread exit: the kernel clears entry.status and wakes joiners.
    call assembler.emit(output, x86_64.xor_reg_reg(7, 7))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_EXIT))
    call assembler.emit(output, x86_64.syscall())
end

function emit_signal_handler(output):
    # SIGUSR1 stop-the-world handler. Runs on the interrupted worker's stack
    # with its registers saved by the kernel in the sigframe, so the
    # collector's conservative scan from the saved rsp up to the stack base
    # sees every live pointer. QBE may use RBX for a local between runtime
    # calls, so recover the state from the worker's dedicated TLS slot. GC
    # signals only entries in the worker table, never the main thread (whose
    # TLS block does not have this slot). rt_sigreturn restores the interrupted
    # RBX from the kernel frame; rsp stays untouched for the restorer.
    call assembler.label(output, "thread_signal_handler")
    call assembler.emit(output, x86_64.load_fs(3, abi_x86_64.TLS_WORKER_STATE))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_GETTID))
    call assembler.emit(output, x86_64.syscall())
    call emit_entry_lookup(output, "handler_found", "handler_return")
    call assembler.emit(output, x86_64.store_memory(9, 24, 4))
    call assembler.emit(output, x86_64.store_memory_imm32(9, 16, 2))
    call assembler.label(output, "handler_gate_wait")
    # futex_wait(&state+128, 1) until the collector drops the gate.
    call assembler.emit(output, x86_64.lea_base(7, 3, 128))
    call assembler.emit(output, x86_64.xor_reg_reg(6, 6))
    call assembler.emit(output, x86_64.mov_imm64(2, 1))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_FUTEX))
    call assembler.emit(output, x86_64.xor_reg_reg(10, 10))
    call assembler.emit(output, x86_64.xor_reg_reg(8, 8))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.load_memory(2, 3, 128))
    call assembler.emit(output, x86_64.cmp_reg_imm8(2, 1))
    call assembler.relative(output, [15, 132], "handler_gate_wait")
    call assembler.emit(output, x86_64.store_memory_imm32(9, 16, 1))
    # `ret` pops the frame return slot into the restorer, which leaves rsp at
    # the kernel ucontext as rt_sigreturn expects.
    call assembler.label(output, "handler_return")
    call assembler.emit(output, x86_64.ret())
    call assembler.label(output, "thread_signal_restorer")
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_RT_SIGRETURN))
    call assembler.emit(output, x86_64.syscall())
end

function emit_join(output):
    # runtime_thread_join: rdi = kernel tid -> 0 once the worker exited, 126
    # if no table entry carries that tid (never spawned, or its record was
    # recycled by a spawn on a full table). Futex-waits on the entry status
    # word with whatever value it holds, then marks the slot reaped so a
    # later spawn may reuse it. Both lookups run under the table mutex, so a
    # slot is never claimed between finding it and waiting on it.
    call assembler.label(output, "runtime_thread_join")
    call assembler.emit(output, x86_64.function_prologue(48))
    call assembler.emit(output, x86_64.store_stack(-8, 7))
    call assembler.relative(output, [232], "runtime_table_lock")
    call assembler.emit(output, x86_64.load_stack(0, -8))
    call emit_entry_lookup(output, "join_found", "join_missing")
    call assembler.emit(output, x86_64.store_stack(-16, 9))
    call assembler.relative(output, [232], "runtime_table_unlock")
    call assembler.label(output, "join_wait")
    # The main thread must still service allocation pressure while waiting
    # for a worker that creates garbage. A bounded futex wait supplies a safe
    # polling point; workers themselves remain non-collecting coordinators.
    call assembler.emit(output, x86_64.load_memory(10, 3, abi_x86_64.GC_ALLOCATED_BYTES))
    call assembler.emit(output, x86_64.load_memory(11, 3, abi_x86_64.GC_BUDGET))
    call assembler.emit(output, x86_64.cmp_reg_reg(10, 11))
    call assembler.relative(output, [15, 140], "join_gc_ready")
    call assembler.relative(output, [232], "runtime_maybe_gc")
    call assembler.label(output, "join_gc_ready")
    call assembler.emit(output, x86_64.load_stack(9, -16))
    call assembler.emit(output, x86_64.load_memory(2, 9, 16))
    call assembler.emit(output, x86_64.cmp_reg_imm8(2, 0))
    call assembler.relative(output, [15, 132], "join_done")
    call assembler.emit(output, x86_64.lea_base(7, 9, 16))
    call assembler.emit(output, x86_64.xor_reg_reg(6, 6))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_FUTEX))
    call assembler.emit(output, x86_64.store_memory_imm32(5, -32, 0))
    call assembler.emit(output, x86_64.store_memory_imm32(5, -24, 10000000))
    call assembler.emit(output, x86_64.lea_base(10, 5, -32))
    call assembler.emit(output, x86_64.xor_reg_reg(8, 8))
    call assembler.emit(output, x86_64.syscall())
    call assembler.relative(output, [233], "join_wait")
    call assembler.label(output, "join_done")
    call assembler.relative(output, [232], "runtime_table_lock")
    call assembler.emit(output, x86_64.load_stack(0, -8))
    call emit_entry_lookup(output, "join_reap", "join_reap_done")
    call assembler.emit(output, x86_64.load_memory(0, 9, 0))
    call assembler.emit(output, x86_64.mov_imm64(11, -9223372036854775808))
    call assembler.emit(output, x86_64.or_reg_reg(0, 11))
    call assembler.emit(output, x86_64.store_memory(9, 0, 0))
    call assembler.label(output, "join_reap_done")
    call assembler.relative(output, [232], "runtime_table_unlock")
    call assembler.emit(output, x86_64.xor_reg_reg(0, 0))
    call assembler.emit(output, x86_64.function_epilogue())
    call assembler.label(output, "join_missing")
    call assembler.relative(output, [232], "runtime_table_unlock")
    call assembler.emit(output, x86_64.mov_imm64(0, 126))
    call assembler.emit(output, x86_64.function_epilogue())
end

function emit_cpu_count(output):
    # runtime_cpu_count: number of CPUs in this thread's affinity mask, or 1
    # when sched_getaffinity fails. Programs use it to size worker pools.
    call assembler.label(output, "runtime_cpu_count")
    call assembler.emit(output, x86_64.function_prologue(144))
    call assembler.emit(output, x86_64.lea_base(2, 5, -136))
    call assembler.emit(output, x86_64.xor_reg_reg(7, 7))
    call assembler.emit(output, x86_64.mov_imm64(6, platform.FUTEX_WAIT_PRIVATE))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_SCHED_GETAFFINITY))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 142], "cpu_count_fallback")
    call assembler.emit(output, x86_64.move_reg_reg(1, 0))
    call assembler.emit(output, x86_64.lea_base(9, 5, -136))
    call assembler.emit(output, x86_64.xor_reg_reg(8, 8))
    call assembler.emit(output, x86_64.xor_reg_reg(10, 10))
    call assembler.label(output, "cpu_count_byte")
    call assembler.emit(output, x86_64.cmp_reg_reg(10, 1))
    call assembler.relative(output, [15, 141], "cpu_count_done")
    call assembler.emit(output, x86_64.load_byte_indexed(11, 9, 10))
    call assembler.label(output, "cpu_count_bit")
    call assembler.emit(output, x86_64.test_reg_reg(11, 11))
    call assembler.relative(output, [15, 132], "cpu_count_next")
    call assembler.emit(output, x86_64.move_reg_reg(0, 11))
    call assembler.emit(output, x86_64.and_reg_imm8(0, 1))
    call assembler.emit(output, x86_64.add_reg_reg(8, 0))
    call assembler.emit(output, x86_64.shift_right_logical_imm(11, 1))
    call assembler.relative(output, [233], "cpu_count_bit")
    call assembler.label(output, "cpu_count_next")
    call assembler.emit(output, x86_64.increment(10))
    call assembler.relative(output, [233], "cpu_count_byte")
    call assembler.label(output, "cpu_count_done")
    call assembler.emit(output, x86_64.move_reg_reg(0, 8))
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 143], "cpu_count_return")
    call assembler.label(output, "cpu_count_fallback")
    call assembler.emit(output, x86_64.mov_imm64(0, 1))
    call assembler.label(output, "cpu_count_return")
    call assembler.emit(output, x86_64.function_epilogue())
end

# QBE calls these leaf services after validating the IntArray and index.
# The slot address is computed after operand evaluation so a growing array's
# backing buffer is never retained across a potentially allocating call.
function emit_atomic(output):
    call assembler.label(output,"runtime_atomic_add")
    call assembler.emit(output,x86_64.move_reg_reg(0,6))
    call assembler.emit(output,x86_64.xor_reg_reg(10,10))
    call assembler.emit(output,x86_64.lock_xadd_indexed(7,10,0,0))
    call assembler.emit(output,x86_64.ret())
    call assembler.label(output,"runtime_atomic_xchg")
    call assembler.emit(output,x86_64.move_reg_reg(0,6))
    call assembler.emit(output,x86_64.xor_reg_reg(10,10))
    call assembler.emit(output,x86_64.lock_xchg_indexed(7,10,0,0))
    call assembler.emit(output,x86_64.ret())
    call assembler.label(output,"runtime_atomic_cas")
    call assembler.emit(output,x86_64.move_reg_reg(0,6))
    call assembler.emit(output,x86_64.xor_reg_reg(10,10))
    call assembler.emit(output,x86_64.lock_cmpxchg_indexed(7,10,0,2))
    call assembler.emit(output,x86_64.ret())
    call assembler.label(output,"runtime_atomic_pause")
    call assembler.emit(output,x86_64.pause())
    call assembler.emit(output,x86_64.xor_reg_reg(0,0))
    call assembler.emit(output,x86_64.ret())
end

function emit_all(output):
    call emit_atomic(output)
    call emit_cpu_count(output)
    call emit_mutex(output)
    call emit_spawn(output)
    call emit_entry(output)
    call emit_signal_handler(output)
    call emit_join(output)
end
