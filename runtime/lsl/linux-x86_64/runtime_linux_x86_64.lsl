# lsl: internal-abi
# Emitor runtime Linux x86_64: společný vstup backendu do cílové implementace.

load runtime_abi_linux_x86_64 as abi_x86_64
load assembler_x86_64 as assembler
load _platform_linux_x86_64 as platform
load native_runtime_linux_x86_64
load thread_runtime_linux_x86_64
load x86_64

function emit_all(output):
    call native_runtime_linux_x86_64.emit_all(output)
    call thread_runtime_linux_x86_64.emit_all(output)
end

function emit_lazy(output, text_used, input_used):
    call native_runtime_linux_x86_64.emit_lazy(output, text_used, input_used)
end

function emit_entry(output, globals_bytes, registry_bytes, registry_mask):
    # r15 carries the initial stack top only until the thread-local block
    # exists; afterwards it is an ordinary callee-saved register.
    call assembler.emit(output, x86_64.move_reg_reg(15, 4))
    call assembler.emit(output, x86_64.function_prologue(16))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_BRK))
    call assembler.emit(output, x86_64.mov_imm64(7, 0))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.move_reg_reg(3, 0))
    # State uses bytes 0..191: 0..87 runtime, 88 native mapping mutex,
    # 96/104 heap/thread-table mutexes, 112 thread
    # table, 120 main stack top, 128 gc gate, 136 threads flag, 160..183 the
    # main thread's thread-local block (fs base: pending error, error flag,
    # stack top), 144 allocated bytes since collection, 152 the adaptive GC
    # byte budget and 184 the GC worklist head; 192..1279 the 136
    # segregated free-list heads (128 exact 8-byte bins up to 1 KiB plus 8
    # range classes) (never GC roots); 1280 the stdin buffering mode and
    # 1288 the stdin buffer (see io_runtime emit_stdin_buffer), 1296/1304 the
    # allocator's brk bump pointer and reserved end (heap_runtime alloc_new),
    # 1312/1320 the stdout buffering mode and buffer (io_runtime
    # emit_stdout_buffer); then the program
    # globals (globals_bytes) and the 1 MiB
    # String-intern table, all on brk; the object registry starts small in
    # mmap.
    call assembler.emit(output, x86_64.lea_base(9, 3, abi_x86_64.STATE_BYTES + globals_bytes))
    call assembler.emit(output, x86_64.move_reg_reg(7, 9))
    call assembler.emit(output, x86_64.add_reg_imm32(7, 1048576))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_BRK))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_reg(0, 7))
    call assembler.relative(output, [15, 133], "runtime_fatal_allocation")
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_MMAP))
    call assembler.emit(output, x86_64.xor_reg_reg(7, 7))
    call assembler.emit(output, x86_64.mov_imm64(6, registry_bytes))
    call assembler.emit(output, x86_64.mov_imm64(2, 3))
    call assembler.emit(output, x86_64.mov_imm64(10, 34))
    call assembler.emit(output, x86_64.mov_imm64(8, -1))
    call assembler.emit(output, x86_64.xor_reg_reg(9, 9))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 140], "runtime_fatal_allocation")
    call assembler.emit(output, x86_64.store_memory(3, 32, 0))
    call assembler.emit(output, x86_64.mov_imm64(10, registry_mask))
    call assembler.emit(output, x86_64.store_memory(3, 40, 10))
    call assembler.emit(output, x86_64.lea_base(9, 3, abi_x86_64.STATE_BYTES + globals_bytes))
    call assembler.emit(output, x86_64.store_memory(3, 48, 9))
    call assembler.emit(output, x86_64.mov_imm64(10, 65535))
    call assembler.emit(output, x86_64.store_memory(3, 56, 10))
    # Bitmapa začátků objektů (state+1328, kapacita +1336): 1 bit na 8 bajtů
    # od začátku stavu, MAP_NORESERVE (fyzicky jen použité stránky). Nahrazuje
    # hashovanou tabulku objektů při rozlišování hodnot Any
    # (runtime_is_heap_object); roste v runtime_object_bitmap_ensure.
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_MMAP))
    call assembler.emit(output, x86_64.xor_reg_reg(7, 7))
    call assembler.emit(output, x86_64.mov_imm64(6, abi_x86_64.OBJECT_BITMAP_INITIAL))
    call assembler.emit(output, x86_64.mov_imm64(2, 3))
    call assembler.emit(output, x86_64.mov_imm64(10, platform.MAP_PRIVATE + platform.MAP_ANONYMOUS + 16384))
    call assembler.emit(output, x86_64.mov_imm64(8, -1))
    call assembler.emit(output, x86_64.xor_reg_reg(9, 9))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 140], "runtime_fatal_allocation")
    call assembler.emit(output, x86_64.store_memory(3, abi_x86_64.OBJECT_BITMAP, 0))
    call assembler.emit(output, x86_64.mov_imm64(0, abi_x86_64.OBJECT_BITMAP_INITIAL))
    call assembler.emit(output, x86_64.store_memory(3, abi_x86_64.OBJECT_BITMAP_CAPACITY, 0))
    # Main-thread identity for gc() and envp lookup: the initial stack top.
    call assembler.emit(output, x86_64.store_memory(3, 120, 15))
    call assembler.emit(output, x86_64.store_memory_imm32(3, abi_x86_64.GC_BUDGET, 4194304))
    # Thread-local block for the main thread: arch_prctl(ARCH_SET_FS, state+160),
    # then fs:16 = stack top (fs:0/fs:8 pending error state start zeroed).
    call assembler.emit(output, x86_64.mov_imm64(7, platform.ARCH_SET_FS))
    call assembler.emit(output, x86_64.lea_base(6, 3, abi_x86_64.MAIN_TLS))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_ARCH_PRCTL))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 140], "runtime_fatal_setup")
    call assembler.emit(output, x86_64.store_fs(abi_x86_64.TLS_STACK_TOP, 15))
    # Thread table: one zeroed page of 32-byte entries (see thread_runtime).
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_MMAP))
    call assembler.emit(output, x86_64.xor_reg_reg(7, 7))
    call assembler.emit(output, x86_64.mov_imm64(6, 8192))
    call assembler.emit(output, x86_64.mov_imm64(2, 3))
    call assembler.emit(output, x86_64.mov_imm64(10, 34))
    call assembler.emit(output, x86_64.mov_imm64(8, -1))
    call assembler.emit(output, x86_64.xor_reg_reg(9, 9))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 140], "runtime_fatal_allocation")
    call assembler.emit(output, x86_64.store_memory(3, 112, 0))
    # SIGUSR1 stop-the-world handler for gc(); it only ever fires in workers.
    # Restart blocking IO/wait4 after a GC suspension; an internal collection
    # must not surface EINTR as a user-visible process or file failure.
    # rt_sigaction(SIGUSR1, {handler, SA_RESTORER|SA_RESTART, restorer, 0}, 0, 8).
    call assembler.emit(output, x86_64.sub_reg_imm(4, platform.SIGACTION_BYTES))
    call assembler.relative(output, x86_64.lea_rip_prefix(0), "thread_signal_handler")
    call assembler.emit(output, x86_64.store_memory(4, platform.SIGACTION_HANDLER, 0))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SA_RESTORER + platform.SA_RESTART))
    call assembler.emit(output, x86_64.store_memory(4, platform.SIGACTION_FLAGS, 0))
    call assembler.relative(output, x86_64.lea_rip_prefix(0), "thread_signal_restorer")
    call assembler.emit(output, x86_64.store_memory(4, platform.SIGACTION_RESTORER, 0))
    call assembler.emit(output, x86_64.xor_reg_reg(0, 0))
    call assembler.emit(output, x86_64.store_memory(4, platform.SIGACTION_MASK, 0))
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_RT_SIGACTION))
    call assembler.emit(output, x86_64.mov_imm64(7, platform.SIGUSR1))
    call assembler.emit(output, x86_64.move_reg_reg(6, 4))
    call assembler.emit(output, x86_64.xor_reg_reg(2, 2))
    call assembler.emit(output, x86_64.mov_imm64(10, platform.SIGSET_BYTES))
    call assembler.emit(output, x86_64.syscall())
    call assembler.emit(output, x86_64.add_reg_imm8(4, platform.SIGACTION_BYTES))
    call assembler.emit(output, x86_64.cmp_reg_imm8(0, 0))
    call assembler.relative(output, [15, 140], "runtime_fatal_setup")
    call assembler.emit(output, x86_64.lea_base(7, 3, abi_x86_64.STATE_BYTES))
    call assembler.emit(output, x86_64.lea_base(6, 3, abi_x86_64.STATE_BYTES + globals_bytes))
    call assembler.relative(output, [232], "runtime_set_global_roots")
end

# Status již musí být v registru argumentu; pořadí flush/release určuje volající.
function emit_exit(output):
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_EXIT_GROUP))
    call assembler.emit(output, x86_64.syscall())
end

function emit_finish(output):
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_EXIT_GROUP))
    call assembler.emit(output, x86_64.mov_imm64(7, 0))
    call assembler.emit(output, x86_64.syscall())
end

# Registry descriptor/pointer/length jsou již připravené podle cílového ABI.
function emit_write_number(output):
    call assembler.emit(output, x86_64.mov_imm64(0, platform.SYS_WRITE))
end

function emit_syscall(output):
    call assembler.emit(output, x86_64.syscall())
end

function matches(target):
    return target.get("os", "") == "linux" and target.get("runtime", "") == platform.RUNTIME and target.get("platform_module", "") == "_platform_linux_x86_64"
end

function state_bytes():
    return abi_x86_64.STATE_BYTES
end

function thread_offset(slot):
    if slot == "error":
        return abi_x86_64.TLS_ERROR
    elif slot == "error_flag":
        return abi_x86_64.TLS_ERROR_FLAG
    elif slot == "stack_top":
        return abi_x86_64.TLS_STACK_TOP
    end
    Error(CompileError: "unknown runtime thread state slot: " + String(slot))
end

# Makra zachovají registry kromě výslovného load destination; compare mění flags.
function thread_load(output, register, slot):
    call assembler.emit(output, x86_64.load_fs(register, thread_offset(slot)))
end

function thread_store(output, slot, register):
    call assembler.emit(output, x86_64.store_fs(thread_offset(slot), register))
end

function thread_store_immediate(output, slot, value):
    call assembler.emit(output, x86_64.store_fs_imm32(thread_offset(slot), value))
end

function thread_compare(output, slot, value):
    call assembler.emit(output, x86_64.cmp_fs_imm8(thread_offset(slot), value))
end

function emit_gc_poll(output, done):
    call assembler.emit(output, x86_64.load_memory(10, 3, abi_x86_64.GC_ALLOCATED_BYTES))
    call assembler.emit(output, x86_64.load_memory(11, 3, abi_x86_64.GC_BUDGET))
    call assembler.emit(output, x86_64.cmp_reg_reg(10, 11))
    call assembler.relative(output, [15, 140], done)
    call assembler.relative(output, [232], "runtime_maybe_gc")
end

# Statický zápis používá privátní LSL ABI: rdi=handle, rsi=data, rdx=bytes.
function prepare_static_write(output):
    call emit_write_number(output)
end

function emit_static_write(output):
    call emit_syscall(output)
end

function release_input(output):
    call assembler.relative(output, [232], "stdin_release")
end
