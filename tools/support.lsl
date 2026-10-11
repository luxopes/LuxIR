# Runtime-independent support for the migrated QBE C interfaces.
function q_assert(raw_condition, expression, file, line):
    condition = __lsl_qbe_raw_int(raw_condition)
    if condition == 0:
        call stdio.fprintf(3, mem.cstr("%s:%d: assertion failed: %s\n"), [file, line, expression])
        call libc.c_abort()
    end
    return 0
end

function q_uint_float(raw_value):
    value = __lsl_qbe_raw_int(raw_value)
    if value >= 0:
        return Float(value)
    end
    return Float(__lsl_qbe_lshr(value, 1)) * 2.0 + Float(value & 1)
end

function q_sort(raw_base, raw_count, raw_size, raw_compare):
    base = __lsl_qbe_raw_int(raw_base)
    count = __lsl_qbe_raw_int(raw_count)
    size = __lsl_qbe_raw_int(raw_size)
    compare = __lsl_qbe_raw_int(raw_compare)
    # Stable merge sorting matches equal-key ordering of the Linux reference.
    # QBE's spill comparators deliberately leave some priorities tied.
    if count < 2:
        return 0
    end
    scratch = __lsl_qbe_raw_int(mem.malloc(count * size))
    source = base
    destination = scratch
    width = 1
    while width < count:
        start = 0
        while start < count:
            middle = min(start + width, count)
            finish = min(start + width * 2, count)
            left = start
            right = middle
            output = start
            while left < middle or right < finish:
                choose_left = right >= finish
                if left < middle and right < finish:
                    choose_left = QBE_COMPARATOR_DISPATCH(compare, source + left * size, source + right * size) <= 0
                end
                if choose_left:
                    element = source + left * size
                    left += 1
                else:
                    element = source + right * size
                    right += 1
                end
                destination_element = destination + output * size
                if size == 4:
                    call __lsl_qbe_store32(destination_element, __lsl_qbe_load32(element))
                elif size == 8:
                    call __lsl_qbe_store64(destination_element, __lsl_qbe_load64(element))
                else:
                    call mem.memcpy(destination_element, element, size)
                end
                output += 1
            end
            start = finish
        end
        previous = source
        source = destination
        destination = previous
        width *= 2
    end
    if source != base:
        call mem.memcpy(base, source, count * size)
    end
    call mem.free(scratch)
    return 0
end

q_getopt_state = {"offset": 1}
function q_getopt_option_span(raw_argc, raw_argv, raw_index, raw_options):
    argc = __lsl_qbe_raw_int(raw_argc)
    argv = __lsl_qbe_raw_int(raw_argv)
    index = __lsl_qbe_raw_int(raw_index)
    options = __lsl_qbe_raw_int(raw_options)
    argument = __lsl_qbe_load64(argv + index * 8)
    offset = 1
    while __lsl_qbe_load8(argument + offset) != 0:
        option = __lsl_qbe_load8(argument + offset)
        match = mem.strchr(options, option)
        if match != 0 and __lsl_qbe_load8(match + 1) == 58:
            if __lsl_qbe_load8(argument + offset + 1) != 0:
                return 1
            end
            if index + 1 < argc:
                return 2
            end
            return 1
        end
        offset += 1
    end
    return 1
end

function q_getopt(raw_argc, raw_argv, raw_options):
    argc = __lsl_qbe_raw_int(raw_argc)
    argv = __lsl_qbe_raw_int(raw_argv)
    options = __lsl_qbe_raw_int(raw_options)
    index = __lsl_qbe_load32(q_g_optind)
    offset = __lsl_typed_int(q_getopt_state["offset"])
    if offset == 1:
        # GNU getopt permutes options ahead of operands, preserving the order
        # of both. Rotate an option and its separate argument as one block.
        if index < argc:
            argument = __lsl_qbe_load64(argv + index * 8)
            if mem.strcmp(argument, mem.cstr("--")) == 0:
                # Keep argc/argv intact: QBE consumes operands up to argc.
                # Removing the marker would introduce a final NULL operand,
                # which QBE interprets as an extra request to read stdin.
                call __lsl_qbe_store32(q_g_optind, index + 1)
                return -1
            end
            if __lsl_qbe_load8(argument) != 45 or __lsl_qbe_load8(argument + 1) == 0:
                scan = index + 1
                option_index = -1
                while scan < argc:
                    candidate = __lsl_qbe_load64(argv + scan * 8)
                    if mem.strcmp(candidate, mem.cstr("--")) == 0:
                        # Permute the marker before the pending operands,
                        # then leave optind immediately after it, like GNU getopt.
                        marker = scan
                        while marker > index:
                            call __lsl_qbe_store64(argv + marker * 8, __lsl_qbe_load64(argv + (marker - 1) * 8))
                            marker -= 1
                        end
                        call __lsl_qbe_store64(argv + index * 8, candidate)
                        call __lsl_qbe_store32(q_g_optind, index + 1)
                        q_getopt_state["offset"] = 1
                        return -1
                    end
                    if __lsl_qbe_load8(candidate) == 45 and __lsl_qbe_load8(candidate + 1) != 0:
                        option_index = scan
                        break
                    end
                    scan += 1
                end
                if option_index >= 0:
                    span = q_getopt_option_span(argc, argv, option_index, options)
                    first = __lsl_qbe_load64(argv + option_index * 8)
                    second = 0
                    if span == 2:
                        second = __lsl_qbe_load64(argv + (option_index + 1) * 8)
                    end
                    cursor = option_index
                    while cursor > index:
                        call __lsl_qbe_store64(argv + (cursor + span - 1) * 8, __lsl_qbe_load64(argv + (cursor - 1) * 8))
                        cursor -= 1
                    end
                    call __lsl_qbe_store64(argv + index * 8, first)
                    if span == 2:
                        call __lsl_qbe_store64(argv + (index + 1) * 8, second)
                    end
                    argument = first
                else:
                    call __lsl_qbe_store32(q_g_optind, index)
                    q_getopt_state["offset"] = 1
                    return -1
                end
            end
        end
    end
    if index >= argc:
        return -1
    end
    argument = __lsl_qbe_load64(argv + index * 8)
    if __lsl_qbe_load8(argument) != 45 or __lsl_qbe_load8(argument + 1) == 0:
        call __lsl_qbe_store32(q_g_optind, index)
        return -1
    end
    offset = __lsl_typed_int(q_getopt_state["offset"])
    option = __lsl_qbe_load8(argument + offset)
    offset += 1
    q_getopt_state["offset"] = offset
    match = mem.strchr(options, option)
    if match == 0 or option == 58:
        call __lsl_qbe_store32(q_g_optopt, option)
        if __lsl_qbe_load8(argument + offset) == 0:
            call __lsl_qbe_store32(q_g_optind, index + 1)
            q_getopt_state["offset"] = 1
        end
        return 63
    end
    if __lsl_qbe_load8(match + 1) == 58:
        if __lsl_qbe_load8(argument + offset) != 0:
            call __lsl_qbe_store64(q_g_optarg, argument + offset)
        elif index + 1 < argc:
            index += 1
            call __lsl_qbe_store64(q_g_optarg, __lsl_qbe_load64(argv + index * 8))
        else:
            call __lsl_qbe_store32(q_g_optopt, option)
            call __lsl_qbe_store32(q_g_optind, index + 1)
            q_getopt_state["offset"] = 1
            if __lsl_qbe_load8(options) == 58:
                return 58
            end
            return 63
        end
        call __lsl_qbe_store32(q_g_optind, index + 1)
        q_getopt_state["offset"] = 1
    elif __lsl_qbe_load8(argument + offset) == 0:
        call __lsl_qbe_store32(q_g_optind, index + 1)
        q_getopt_state["offset"] = 1
    end
    return option
end

# The original phi insertion loop scans every instruction for each temporary.
# A single definition with no live-in and only dominated non-phi uses cannot
# need a merge or local rename. All original fallback behavior is retained.
# Layout placeholders are generated from QBE's verified C types.
function q_ssa_unique_definition_safe(raw_fn, raw_t, raw_defb):
    fn = __lsl_qbe_raw_int(raw_fn)
    t = __lsl_qbe_raw_int(raw_t)
    defb = __lsl_qbe_raw_int(raw_defb)
    rpo = __lsl_qbe_load64(fn + QBE_FN_RPO)
    definition = __lsl_qbe_load64(rpo + defb * 8)
    incoming = __lsl_qbe_load64(definition + QBE_BLOCK_IN + QBE_BITSET_DATA)
    bits = __lsl_qbe_load64(incoming + (t // 64) * 8)
    if bits & __lsl_qbe_shl(1, t % 64):
        return 0
    end
    temporary = __lsl_qbe_load64(fn + QBE_FN_TMP) + t * QBE_TMP_SIZE
    use = __lsl_qbe_load64(temporary + QBE_TMP_USES)
    count = __lsl_qbe_load32(temporary + QBE_TMP_NUSE)
    for index in rang(count):
        item = use + index * QBE_USE_SIZE
        if __lsl_qbe_load32(item + QBE_USE_TYPE) == QBE_USE_PHI:
            return 0
        end
        block_id = __lsl_qbe_load32(item + QBE_USE_BLOCK)
        if block_id != defb:
            block = __lsl_qbe_load64(rpo + block_id * 8)
            if q_f_sdom(definition, block) == 0:
                return 0
            end
        end
    end
    # The original nonempty definition worklist marks this temporary for
    # renblk even if it inserts no phi. Preserve renaming and temporary order.
    call __lsl_qbe_store32(temporary + QBE_TMP_VISIT, t)
    return 1
end

# An index entry has head/tail pointers; nodes contain next/block/first ins.
# Nodes follow original block link order, and belong to the PFn arena.
function q_ssa_definition_index(raw_fn):
    fn = __lsl_qbe_raw_int(raw_fn)
    nt = __lsl_qbe_load32(fn + QBE_FN_NTMP)
    table = mem.function_alloc(nt * 16)
    block = __lsl_qbe_load64(fn + QBE_FN_START)
    while block != 0:
        ins = __lsl_qbe_load64(block + QBE_BLOCK_INS)
        limit = ins + __lsl_qbe_load32(block + QBE_BLOCK_NINS) * QBE_INS_SIZE
        while ins < limit:
            ref = __lsl_qbe_load32(ins + QBE_INS_TO)
            if ref != 0 and (ref & 7) == 0:
                temporary = __lsl_qbe_lshr(ref, 3)
                if temporary >= QBE_TMP_FIRST and temporary < nt:
                    entry = table + temporary * 16
                    tail = __lsl_qbe_load64(entry + 8)
                    if tail == 0 or __lsl_qbe_load64(tail + 8) != block:
                        node = mem.function_alloc(24)
                        __lsl_qbe_store64(node + 8, block)
                        __lsl_qbe_store64(node + 16, ins)
                        if tail != 0:
                            __lsl_qbe_store64(tail, node)
                        else:
                            __lsl_qbe_store64(entry, node)
                        end
                        __lsl_qbe_store64(entry + 8, node)
                    end
                end
            end
            ins += QBE_INS_SIZE
        end
        block = __lsl_qbe_load64(block + QBE_BLOCK_LINK)
    end
    return table
end

function q_ssa_clear_block_visits(raw_fn):
    fn = __lsl_qbe_raw_int(raw_fn)
    block = __lsl_qbe_load64(fn + QBE_FN_START)
    while block != 0:
        __lsl_qbe_store32(block + QBE_BLOCK_VISIT, 0)
        block = __lsl_qbe_load64(block + QBE_BLOCK_LINK)
    end
    return 0
end
