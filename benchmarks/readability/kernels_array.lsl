# lsl: internal-abi
# A deliberately separate experiment: ordinary indexed IntArray storage.
# Unlike readable.lsl, this changes the representation/access abstraction.
function q_f_bscount(words):
    index = 0
    total = 0
    while index < len(words):
        word = words[index]
        word = (word & 6148914691236517205) + (__lsl_qbe_lshr(word, 1) & 6148914691236517205)
        word = (word & 3689348814741910323) + (__lsl_qbe_lshr(word, 2) & 3689348814741910323)
        word = (word & 1085102592571150095) + (__lsl_qbe_lshr(word, 4) & 1085102592571150095)
        word += __lsl_qbe_lshr(word, 8)
        word += __lsl_qbe_lshr(word, 16)
        word += __lsl_qbe_lshr(word, 32)
        total = __lsl_qbe_u32(total + (word & 255))
        index += 1
    end
    return total
end

function q_f_bsequal(left, right):
    if len(left) != len(right):
        Error(ValueError: "bitset size mismatch")
    end
    index = 0
    while index < len(left):
        if left[index] != right[index]:
            return 0
        end
        index += 1
    end
    return 1
end

function q_f_bsset(words, raw_element):
    element = __lsl_qbe_raw_int(raw_element)
    if __lsl_qbe_ult(element, len(words) * 64) == 0:
        Error(ValueError: "bitset element out of range")
    end
    index = __lsl_qbe_u32(__lsl_qbe_udiv(element, 64))
    mask = __lsl_qbe_shl(1, __lsl_qbe_u32(__lsl_qbe_urem(element, 64)))
    words[index] = words[index] | mask
    return 0
end

function q_f_bsclr(words, raw_element):
    element = __lsl_qbe_raw_int(raw_element)
    if __lsl_qbe_ult(element, len(words) * 64) == 0:
        Error(ValueError: "bitset element out of range")
    end
    index = __lsl_qbe_u32(__lsl_qbe_udiv(element, 64))
    mask = __lsl_qbe_shl(1, __lsl_qbe_u32(__lsl_qbe_urem(element, 64)))
    words[index] = words[index] & (mask ~ -1)
    return 0
end
