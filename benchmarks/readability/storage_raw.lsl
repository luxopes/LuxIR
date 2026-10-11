# lsl: internal-abi
load luxir_memory as mem
function make_set(raw_count, raw_offset):
    count = __lsl_qbe_raw_int(raw_count)
    offset = __lsl_qbe_raw_int(raw_offset)
    set = mem.malloc(16)
    words = mem.malloc(count * 8)
    call __lsl_qbe_store32(set, count)
    call __lsl_qbe_store64(set + 8, words)
    index = 0
    while index < count:
        call __lsl_qbe_store64(words + index * 8, ((index + offset) * 1234567) ~ -6148914691236517206)
        index += 1
    end
    return set
end
function read_word(raw_set, raw_index):
    set = __lsl_qbe_raw_int(raw_set)
    index = __lsl_qbe_raw_int(raw_index)
    return __lsl_qbe_load64(__lsl_qbe_load64(set + 8) + index * 8)
end
function write_word(raw_set, raw_index, raw_word):
    set = __lsl_qbe_raw_int(raw_set)
    index = __lsl_qbe_raw_int(raw_index)
    call __lsl_qbe_store64(__lsl_qbe_load64(set + 8) + index * 8, __lsl_qbe_raw_int(raw_word))
    return 0
end
