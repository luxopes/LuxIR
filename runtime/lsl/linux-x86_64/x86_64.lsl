# lsl: internal-abi
# Minimal auditable x86_64 encoder. Every instruction is emitted as List[Int];
# no textual assembly and no external assembler are involved.

load bytes

function rex(w, r, x, b):
    return 64 + Bool(w) * 8 + Bool(r) * 4 + Bool(x) * 2 + Bool(b)
end

function modrm(mod, reg, rm):
    return mod * 64 + (reg % 8) * 8 + (rm % 8)
end

function fits_signed8(value):
    return value >= -128 and value <= 127
end

function fits_signed32(value):
    return value >= -2147483648 and value <= 2147483647
end

function mov_imm64(register, value):
    # Prefer shorter encodings while preserving the full signed Int64 value.
    # mov r32,imm32 zero-extends; C7 /0 sign-extends imm32 to r64.
    if value >= 0 and value <= 4294967295:
        return mov_imm32(register, value)
    elif fits_signed32(value):
        result = [rex(True, False, False, register >= 8), 199, modrm(3, 0, register)]
        call bytes.append_little(result, value, 4)
        return result
    end
    result = [rex(True, False, False, register >= 8), 184 + register % 8]
    call bytes.append_little(result, value, 8)
    return result
end

function mov_imm32(register, value):
    result = []
    if register >= 8:
        result.append(rex(False, False, False, True))
    end
    result.append(184 + register % 8)
    call bytes.append_little(result, value, 4)
    return result
end

function lea_rip_prefix(register):
    return [rex(True, register >= 8, False, False), 141, modrm(0, register, 5)]
end

function lea_rip(register, displacement):
    result = lea_rip_prefix(register)
    call bytes.append_little(result, displacement, 4)
    return result
end

function syscall():
    return [15, 5]
end

function ret():
    return [195]
end

function push(register):
    result = []
    if register >= 8:
        result.append(65)
    end
    result.append(80 + register % 8)
    return result
end

function pop(register):
    result = []
    if register >= 8:
        result.append(65)
    end
    result.append(88 + register % 8)
    return result
end

function add_reg_reg(destination, source):
    return [rex(True, source >= 8, False, destination >= 8), 1, modrm(3, source, destination)]
end

function sub_reg_reg(destination, source):
    return [rex(True, source >= 8, False, destination >= 8), 41, modrm(3, source, destination)]
end

function imul_reg_reg(destination, source):
    return [rex(True, destination >= 8, False, source >= 8), 15, 175, modrm(3, destination, source)]
end

function imul_reg_imm(destination, source, value):
    if fits_signed8(value):
        return [rex(True, destination >= 8, False, source >= 8), 107, modrm(3, destination, source), value % 256]
    end
    result = [rex(True, destination >= 8, False, source >= 8), 105, modrm(3, destination, source)]
    call bytes.append_little(result, value, 4)
    return result
end

function cmp_reg_reg(left, right):
    return [rex(True, right >= 8, False, left >= 8), 57, modrm(3, right, left)]
end

function test_reg_reg(left, right):
    return [rex(True, right >= 8, False, left >= 8), 133, modrm(3, right, left)]
end

function xor_reg_reg(destination, source):
    return [rex(True, source >= 8, False, destination >= 8), 49, modrm(3, source, destination)]
end

function and_reg_reg(destination, source):
    return [rex(True, source >= 8, False, destination >= 8), 33, modrm(3, source, destination)]
end

function or_reg_reg(destination, source):
    return [rex(True, source >= 8, False, destination >= 8), 9, modrm(3, source, destination)]
end

function call_relative(displacement):
    result = [232]
    call bytes.append_little(result, displacement, 4)
    return result
end

function call_reg(register):
    return [rex(True, False, False, register >= 8), 255, modrm(3, 2, register)]
end

function movq_xmm_from_reg(xmm_register, integer_register):
    return [102, rex(True, xmm_register >= 8, False, integer_register >= 8), 15, 110, modrm(3, xmm_register, integer_register)]
end

function movq_reg_from_xmm(integer_register, xmm_register):
    return [102, rex(True, xmm_register >= 8, False, integer_register >= 8), 15, 126, modrm(3, xmm_register, integer_register)]
end

function scalar_double(operation, destination, source):
    return [242, rex(False, destination >= 8, False, source >= 8), 15, operation, modrm(3, destination, source)]
end

function add_double(destination, source):
    return scalar_double(88, destination, source)
end

function sub_double(destination, source):
    return scalar_double(92, destination, source)
end

function mul_double(destination, source):
    return scalar_double(89, destination, source)
end

function div_double(destination, source):
    return scalar_double(94, destination, source)
end

function sqrt_double(destination, source):
    return scalar_double(81, destination, source)
end

function compare_double(left, right):
    return [102, rex(False, left >= 8, False, right >= 8), 15, 46, modrm(3, left, right)]
end

function int_to_double(xmm_register, integer_register):
    return [242, rex(True, xmm_register >= 8, False, integer_register >= 8), 15, 42, modrm(3, xmm_register, integer_register)]
end

function double_to_int(integer_register, xmm_register):
    return [242, rex(True, integer_register >= 8, False, xmm_register >= 8), 15, 44, modrm(3, integer_register, xmm_register)]
end

function packed_double(operation, destination, source):
    # SSE2 packed-double register operation (66 0F op /r) on two lanes.
    return [102, rex(False, destination >= 8, False, source >= 8), 15, operation, modrm(3, destination, source)]
end

function add_packed(destination, source):
    return packed_double(88, destination, source)
end

function mul_packed(destination, source):
    return packed_double(89, destination, source)
end

function move_packed(destination, source):
    # movapd xmm, xmm
    return packed_double(40, destination, source)
end

function unpack_low_packed(destination, source):
    # unpcklpd: destination = [destination.low, source.low]; with the same
    # register on both sides this broadcasts the low lane.
    return packed_double(20, destination, source)
end

function unpack_high_packed(destination, source):
    # unpckhpd: destination = [destination.high, source.high]; with the same
    # register on both sides this moves the high lane down.
    return packed_double(21, destination, source)
end

function load_packed_indexed(xmm_register, base_register, index_register, scale):
    # movupd xmm, [base + index*2^scale]: two unaligned doubles.
    return [102, rex(False, xmm_register >= 8, index_register >= 8, base_register >= 8), 15, 16, modrm(0, xmm_register, 4), sib(scale, index_register, base_register)]
end

function store_packed_indexed(base_register, index_register, scale, xmm_register):
    # movupd [base + index*2^scale], xmm
    return [102, rex(False, xmm_register >= 8, index_register >= 8, base_register >= 8), 15, 17, modrm(0, xmm_register, 4), sib(scale, index_register, base_register)]
end

function jump_relative(displacement):
    result = [233]
    call bytes.append_little(result, displacement, 4)
    return result
end

function jump_condition(condition_opcode, displacement):
    result = [15, condition_opcode]
    call bytes.append_little(result, displacement, 4)
    return result
end

function move_reg_reg(destination, source):
    return [rex(True, source >= 8, False, destination >= 8), 137, modrm(3, source, destination)]
end

function load_stack(register, displacement):
    mode = 2
    if fits_signed8(displacement):
        mode = 1
    end
    result = [rex(True, register >= 8, False, False), 139, modrm(mode, register, 5)]
    if mode == 1:
        result.append(displacement % 256)
    else:
        call bytes.append_little(result, displacement, 4)
    end
    return result
end

function store_stack(displacement, register):
    mode = 2
    if fits_signed8(displacement):
        mode = 1
    end
    result = [rex(True, register >= 8, False, False), 137, modrm(mode, register, 5)]
    if mode == 1:
        result.append(displacement % 256)
    else:
        call bytes.append_little(result, displacement, 4)
    end
    return result
end

function function_prologue(frame_size):
    result = [85, 72, 137, 229]
    if frame_size > 0:
        if fits_signed8(frame_size):
            result.extend([72, 131, 236, frame_size])
        else:
            result.extend([72, 129, 236])
            call bytes.append_little(result, frame_size, 4)
        end
    end
    return result
end

function function_epilogue():
    return [201, 195]
end

function add_reg_imm8(register, value):
    return [rex(True, False, False, register >= 8), 131, modrm(3, 0, register), value % 256]
end

function add_reg_imm32(register, value):
    if fits_signed8(value):
        return add_reg_imm8(register, value)
    end
    result = [rex(True, False, False, register >= 8), 129, modrm(3, 0, register)]
    call bytes.append_little(result, value, 4)
    return result
end

function sub_reg_imm(register, value):
    if fits_signed8(value):
        return [rex(True, False, False, register >= 8), 131, modrm(3, 5, register), value % 256]
    end
    result = [rex(True, False, False, register >= 8), 129, modrm(3, 5, register)]
    call bytes.append_little(result, value, 4)
    return result
end

function cmp_reg_imm8(register, value):
    return [rex(True, False, False, register >= 8), 131, modrm(3, 7, register), value % 256]
end

function cmp_reg_imm32(register, value):
    if fits_signed8(value):
        return cmp_reg_imm8(register, value)
    end
    result = [rex(True, False, False, register >= 8), 129, modrm(3, 7, register)]
    call bytes.append_little(result, value, 4)
    return result
end

function logic_reg_imm(operation, register, value):
    # and/or/xor r64, imm (operation: 4 = and, 1 = or, 6 = xor).
    if fits_signed8(value):
        return [rex(True, False, False, register >= 8), 131, modrm(3, operation, register), value % 256]
    end
    result = [rex(True, False, False, register >= 8), 129, modrm(3, operation, register)]
    call bytes.append_little(result, value, 4)
    return result
end

function neg_reg(register):
    return [rex(True, False, False, register >= 8), 247, modrm(3, 3, register)]
end

function div_reg(register):
    return [rex(True, False, False, register >= 8), 247, modrm(3, 6, register)]
end

function idiv_reg(register):
    return [rex(True, False, False, register >= 8), 247, modrm(3, 7, register)]
end
function imul_high_reg(register):
    # one-operand IMUL: RDX:RAX = RAX * register (signed); high half in RDX.
    return [rex(True, False, False, register >= 8), 247, modrm(3, 5, register)]
end
function shift_right_logical_imm(register, amount):
    return [rex(True, False, False, register >= 8), 193, modrm(3, 5, register), amount]
end

function cqo():
    return [72, 153]
end

function increment(register):
    return [rex(True, False, False, register >= 8), 255, modrm(3, 0, register)]
end

function decrement(register):
    return [rex(True, False, False, register >= 8), 255, modrm(3, 1, register)]
end

function store_byte_imm(base_register, value):
    return [rex(False, False, False, base_register >= 8), 198, modrm(0, 0, base_register), value % 256]
end

function store_byte_reg(base_register, source_register):
    return [rex(False, source_register >= 8, False, base_register >= 8), 136, modrm(0, source_register, base_register)]
end

function set_condition(condition_opcode):
    # setcc al; movzx rax, al
    return [15, condition_opcode, 192, 72, 15, 182, 192]
end
function set_condition_ordered(condition_opcode):
    # setcc cl; setnp al; and al,cl; movzx rax,al -> true only if cc and ordered
    return [15, condition_opcode, 193, 15, 155, 192, 32, 200, 72, 15, 182, 192]
end
function set_condition_unordered(condition_opcode):
    # setcc cl; setp al; or al,cl; movzx rax,al -> true if cc or unordered (NaN)
    return [15, condition_opcode, 193, 15, 154, 192, 8, 200, 72, 15, 182, 192]
end

function and_reg_imm8(register, value):
    return [rex(True, False, False, register >= 8), 131, modrm(3, 4, register), value % 256]
end

function shift_left_imm(register, amount):
    return [rex(True, False, False, register >= 8), 193, modrm(3, 4, register), amount]
end

function shift_right_arithmetic_imm(register, amount):
    return [rex(True, False, False, register >= 8), 193, modrm(3, 7, register), amount]
end

function not_reg(register):
    # NOT r/m64: one's complement in place.
    return [rex(True, False, False, register >= 8), 247, modrm(3, 2, register)]
end

function shift_left_cl(register):
    # SHL r/m64, CL. The CPU masks CL to 6 bits, so a count outside 0..63 must
    # already have been handled by the caller.
    return [rex(True, False, False, register >= 8), 211, modrm(3, 4, register)]
end

function shift_right_logical_cl(register):
    # SHR r/m64, CL. Stejné omezení počtu (6 bitů) jako shift_left_cl.
    return [rex(True, False, False, register >= 8), 211, modrm(3, 5, register)]
end

function multiply_unsigned(register):
    # MUL r/m64: rdx:rax = rax * register (bez znaménka, celých 128 bitů).
    return [rex(True, False, False, register >= 8), 247, modrm(3, 4, register)]
end

function bit_scan_reverse(destination, source):
    # BSR r64, r64: index nejvyššího nastaveného bitu (zdroj nesmí být 0).
    return [rex(True, destination >= 8, False, source >= 8), 15, 189, modrm(3, destination, source)]
end

function shift_right_arithmetic_cl(register):
    # SAR r/m64, CL. Same 6-bit masking caveat as shift_left_cl.
    return [rex(True, False, False, register >= 8), 211, modrm(3, 7, register)]
end

function memory_tail(base_register, mode, reg_field):
    result = [modrm(mode, reg_field, base_register)]
    if base_register % 8 == 4:
        result.append(36)
    end
    return result
end

function memory_mode(base_register, displacement):
    if displacement == 0 and base_register % 8 != 5:
        return 0
    elif fits_signed8(displacement):
        return 1
    end
    return 2
end

function append_displacement(result, mode, displacement):
    if mode == 1:
        result.append(displacement % 256)
    elif mode == 2:
        call bytes.append_little(result, displacement, 4)
    end
end

function load_memory(destination, base_register, displacement):
    mode = memory_mode(base_register, displacement)
    result = [rex(True, destination >= 8, False, base_register >= 8), 139]
    result.extend(memory_tail(base_register, mode, destination))
    call append_displacement(result, mode, displacement)
    return result
end

function cmp_reg_memory(register, base_register, displacement):
    mode = memory_mode(base_register, displacement)
    result = [rex(True, register >= 8, False, base_register >= 8), 59]
    result.extend(memory_tail(base_register, mode, register))
    call append_displacement(result, mode, displacement)
    return result
end

function store_memory(base_register, displacement, source_register):
    mode = memory_mode(base_register, displacement)
    result = [rex(True, source_register >= 8, False, base_register >= 8), 137]
    result.extend(memory_tail(base_register, mode, source_register))
    call append_displacement(result, mode, displacement)
    return result
end

function store_memory_imm32(base_register, displacement, value):
    mode = memory_mode(base_register, displacement)
    result = [rex(True, False, False, base_register >= 8), 199]
    result.extend(memory_tail(base_register, mode, 0))
    call append_displacement(result, mode, displacement)
    call bytes.append_little(result, value, 4)
    return result
end

function cmp_memory_imm8(base_register, displacement, value):
    mode = memory_mode(base_register, displacement)
    result = [rex(True, False, False, base_register >= 8), 131]
    result.extend(memory_tail(base_register, mode, 7))
    call append_displacement(result, mode, displacement)
    result.append(value & 255)
    return result
end

function lea_base(destination, base_register, displacement):
    mode = memory_mode(base_register, displacement)
    result = [rex(True, destination >= 8, False, base_register >= 8), 141]
    result.extend(memory_tail(base_register, mode, destination))
    call append_displacement(result, mode, displacement)
    return result
end

function load_byte_zero_extend(destination, base_register, displacement):
    mode = memory_mode(base_register, displacement)
    result = [rex(True, destination >= 8, False, base_register >= 8), 15, 182]
    result.extend(memory_tail(base_register, mode, destination))
    call append_displacement(result, mode, displacement)
    return result
end

function store_byte_reg_disp(base_register, displacement, source_register):
    mode = memory_mode(base_register, displacement)
    result = [rex(False, source_register >= 8, False, base_register >= 8), 136]
    result.extend(memory_tail(base_register, mode, source_register))
    call append_displacement(result, mode, displacement)
    return result
end

function sib(scale, index_register, base_register):
    return scale * 64 + (index_register % 8) * 8 + (base_register % 8)
end

function load_indexed(destination, base_register, index_register, scale):
    return [rex(True, destination >= 8, index_register >= 8, base_register >= 8), 139, modrm(0, destination, 4), sib(scale, index_register, base_register)]
end

function store_indexed(base_register, index_register, scale, source_register):
    return [rex(True, source_register >= 8, index_register >= 8, base_register >= 8), 137, modrm(0, source_register, 4), sib(scale, index_register, base_register)]
end

function load_byte_indexed(destination, base_register, index_register):
    return [rex(True, destination >= 8, index_register >= 8, base_register >= 8), 15, 182, modrm(0, destination, 4), sib(0, index_register, base_register)]
end

function store_byte_indexed(base_register, index_register, source_register):
    return [rex(False, source_register >= 8, index_register >= 8, base_register >= 8), 136, modrm(0, source_register, 4), sib(0, index_register, base_register)]
end

function store_dword_indexed(base_register, index_register, scale, source_register):
    # 32-bit store of the source's low dword: mov [base + index*2^scale], r32.
    # Base register must not be rsp/rbp-class (no displacement path here).
    return [rex(False, source_register >= 8, index_register >= 8, base_register >= 8), 137, modrm(0, source_register, 4), sib(scale, index_register, base_register)]
end

function load_dword_indexed(destination, base_register, index_register, scale):
    # 32-bit load, zero-extended into the full register:
    # mov r32, [base + index*2^scale].
    return [rex(False, destination >= 8, index_register >= 8, base_register >= 8), 139, modrm(0, destination, 4), sib(scale, index_register, base_register)]
end

function lea_indexed(destination, base_register, index_register, scale):
    return [rex(True, destination >= 8, index_register >= 8, base_register >= 8), 141, modrm(0, destination, 4), sib(scale, index_register, base_register)]
end

function alu_mem_indexed(opcode, base_register, index_register, scale, source_register):
    # op [base + index*2^scale], r64 with a memory destination (opcode 1 add,
    # 41 sub, 33 and, 9 or, 49 xor).
    return [rex(True, source_register >= 8, index_register >= 8, base_register >= 8), opcode, modrm(0, source_register, 4), sib(scale, index_register, base_register)]
end

function lock_xadd_indexed(base_register, index_register, scale, source_register):
    # lock xadd [base + index*2^scale], r64. Atomically adds the register to
    # the memory slot; the previous slot value ends up in the register.
    return [240, rex(True, source_register >= 8, index_register >= 8, base_register >= 8), 15, 193, modrm(0, source_register, 4), sib(scale, index_register, base_register)]
end

function lock_xchg_indexed(base_register, index_register, scale, source_register):
    # lock xchg [base + index*2^scale], r64. Atomically exchanges the register
    # with the memory slot; the previous slot value ends up in the register.
    return [240, rex(True, source_register >= 8, index_register >= 8, base_register >= 8), 135, modrm(0, source_register, 4), sib(scale, index_register, base_register)]
end

function lock_cmpxchg_indexed(base_register, index_register, scale, source_register):
    # lock cmpxchg [base + index*2^scale], r64. Atomically compares RAX with
    # the slot: on match it stores the register and sets ZF, otherwise it
    # loads the slot into RAX and clears ZF.
    return [240, rex(True, source_register >= 8, index_register >= 8, base_register >= 8), 15, 177, modrm(0, source_register, 4), sib(scale, index_register, base_register)]
end

function lock_xadd_memory(base_register, displacement, source_register):
    # lock xadd [base + displacement], r64 for fixed state words.
    mode = memory_mode(base_register, displacement)
    result = [240, rex(True, source_register >= 8, False, base_register >= 8), 15, 193]
    result.extend(memory_tail(base_register, mode, source_register))
    call append_displacement(result, mode, displacement)
    return result
end

function lock_xchg_memory(base_register, displacement, source_register):
    # lock xchg [base + displacement], r64 for fixed state words. The previous
    # word value ends up in the source register.
    mode = memory_mode(base_register, displacement)
    result = [240, rex(True, source_register >= 8, False, base_register >= 8), 135]
    result.extend(memory_tail(base_register, mode, source_register))
    call append_displacement(result, mode, displacement)
    return result
end

function lock_cmpxchg_memory(base_register, displacement, source_register):
    # lock cmpxchg [base + displacement], r64 for fixed state words: on a
    # match with RAX it stores the register and sets ZF, otherwise it loads
    # the word into RAX and clears ZF.
    mode = memory_mode(base_register, displacement)
    result = [240, rex(True, source_register >= 8, False, base_register >= 8), 15, 177]
    result.extend(memory_tail(base_register, mode, source_register))
    call append_displacement(result, mode, displacement)
    return result
end

function popcount_reg(destination, source):
    # popcnt r64, r64 (F3 REX.W 0F B8 /r).
    return [243, rex(True, destination >= 8, False, source >= 8), 15, 184, modrm(3, destination, source)]
end

function trailing_zeros_reg(destination, source):
    # tzcnt r64, r64 (F3 REX.W 0F BC /r); 64 for a zero source.
    return [243, rex(True, destination >= 8, False, source >= 8), 15, 188, modrm(3, destination, source)]
end

function load_fs(register, displacement):
    # mov r64, fs:[displacement] (thread-local block, absolute disp32).
    result = [100, rex(True, register >= 8, False, False), 139, modrm(0, register, 4), 37]
    call bytes.append_little(result, displacement, 4)
    return result
end

function store_fs(displacement, register):
    # mov fs:[displacement], r64
    result = [100, rex(True, register >= 8, False, False), 137, modrm(0, register, 4), 37]
    call bytes.append_little(result, displacement, 4)
    return result
end

function store_fs_imm32(displacement, value):
    # mov qword fs:[displacement], imm32 (sign-extended)
    result = [100, 72, 199, modrm(0, 0, 4), 37]
    call bytes.append_little(result, displacement, 4)
    call bytes.append_little(result, value, 4)
    return result
end

function cmp_fs_imm8(displacement, value):
    # cmp qword fs:[displacement], imm8
    result = [100, 72, 131, modrm(0, 7, 4), 37]
    call bytes.append_little(result, displacement, 4)
    result.append(value % 256)
    return result
end

function pause():
    # PAUSE hint for spin-wait loops.
    return [243, 144]
end
