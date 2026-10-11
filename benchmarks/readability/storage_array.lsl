# lsl: internal-abi
function make_set(raw_count, raw_offset):
    count = __lsl_qbe_raw_int(raw_count)
    offset = __lsl_qbe_raw_int(raw_offset)
    words = __lsl_int_zeros(count)
    index = 0
    while index < count:
        words[index] = ((index + offset) * 1234567) ~ -6148914691236517206
        index += 1
    end
    return words
end
function read_word(words, raw_index):
    return words[__lsl_qbe_raw_int(raw_index)]
end
function write_word(words, raw_index, raw_word):
    words[__lsl_qbe_raw_int(raw_index)] = __lsl_qbe_raw_int(raw_word)
    return 0
end
