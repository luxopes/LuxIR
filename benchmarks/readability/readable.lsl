# Experimental replacements only; not imported by the production backend.
# Same BSet layout: uint32 word_count at +0, uint64* words at +8.
# Same unsigned conversions, assertions, popcount algorithm and unrolling.

function q_f_bscount(raw_set):
    set = __lsl_qbe_raw_int(raw_set)
    index = 0
    total = 0
    while __lsl_qbe_ult(index, __lsl_qbe_u32(__lsl_qbe_load32(set))):
        words = __lsl_qbe_load64(set + 8)
        word = __lsl_qbe_load64(words + index * 8)
        word = (word & 6148914691236517205) + (__lsl_qbe_lshr(word, 1) & 6148914691236517205)
        word = (word & 3689348814741910323) + (__lsl_qbe_lshr(word, 2) & 3689348814741910323)
        word = (word & 1085102592571150095) + (__lsl_qbe_lshr(word, 4) & 1085102592571150095)
        word += __lsl_qbe_lshr(word, 8)
        word += __lsl_qbe_lshr(word, 16)
        word += __lsl_qbe_lshr(word, 32)
        total = __lsl_qbe_u32(total + (word & 255))
        index = __lsl_qbe_u32(index + 1)
    end
    return total
end

function q_f_bsequal(raw_left, raw_right):
    left = __lsl_qbe_raw_int(raw_left)
    right = __lsl_qbe_raw_int(raw_right)
    if __lsl_qbe_u32(__lsl_qbe_load32(left)) != __lsl_qbe_u32(__lsl_qbe_load32(right)):
        call q_assert(0, q_string_49, q_string_31, 670)
    end
    index = 0
    while __lsl_qbe_ult(index, __lsl_qbe_u32(__lsl_qbe_load32(left))):
        left_words = __lsl_qbe_load64(left + 8)
        right_words = __lsl_qbe_load64(right + 8)
        if __lsl_qbe_load64(left_words + index * 8) != __lsl_qbe_load64(right_words + index * 8):
            return 0
        end
        index = __lsl_qbe_u32(index + 1)
    end
    return 1
end

function q_f_bsset(raw_set, raw_element):
    set = __lsl_qbe_raw_int(raw_set)
    element = __lsl_qbe_raw_int(raw_element)
    capacity = __lsl_qbe_u32(__lsl_qbe_u32(__lsl_qbe_load32(set)) * 64)
    if __lsl_qbe_ult(element, capacity) == 0:
        call q_assert(0, q_string_48, q_string_31, 638)
    end
    words = __lsl_qbe_load64(set + 8)
    address = words + __lsl_qbe_u32(__lsl_qbe_udiv(element, 64)) * 8
    mask = __lsl_qbe_shl(1, __lsl_qbe_u32(__lsl_qbe_urem(element, 64)))
    call __lsl_qbe_store64(address, __lsl_qbe_load64(address) | mask)
    return 0
end

function q_f_bsclr(raw_set, raw_element):
    set = __lsl_qbe_raw_int(raw_set)
    element = __lsl_qbe_raw_int(raw_element)
    capacity = __lsl_qbe_u32(__lsl_qbe_u32(__lsl_qbe_load32(set)) * 64)
    if __lsl_qbe_ult(element, capacity) == 0:
        call q_assert(0, q_string_48, q_string_31, 645)
    end
    words = __lsl_qbe_load64(set + 8)
    address = words + __lsl_qbe_u32(__lsl_qbe_udiv(element, 64)) * 8
    mask = __lsl_qbe_shl(1, __lsl_qbe_u32(__lsl_qbe_urem(element, 64)))
    call __lsl_qbe_store64(address, __lsl_qbe_load64(address) & (mask ~ -1))
    return 0
end

function q_f_bscopy(raw_destination, raw_source):
    destination_set = __lsl_qbe_raw_int(raw_destination)
    source_set = __lsl_qbe_raw_int(raw_source)
    count = __lsl_qbe_load32(destination_set + 0)
    if count != __lsl_qbe_load32(source_set + 0):
        call q_assert(0, q_string_49, q_string_31, 660)
    end
    destination = __lsl_qbe_load64(destination_set + 8)
    source = __lsl_qbe_load64(source_set + 8)
    index = 0
    while index + 4 <= count:
        call __lsl_qbe_store64(destination + (index + 0) * 8, __lsl_qbe_load64(source + (index + 0) * 8))
        call __lsl_qbe_store64(destination + (index + 1) * 8, __lsl_qbe_load64(source + (index + 1) * 8))
        call __lsl_qbe_store64(destination + (index + 2) * 8, __lsl_qbe_load64(source + (index + 2) * 8))
        call __lsl_qbe_store64(destination + (index + 3) * 8, __lsl_qbe_load64(source + (index + 3) * 8))
        index += 4
    end
    while index < count:
        call __lsl_qbe_store64(destination + (index + 0) * 8, __lsl_qbe_load64(source + (index + 0) * 8))
        index += 1
    end
    return 0
end

function q_f_bsunion(raw_destination, raw_source):
    destination_set = __lsl_qbe_raw_int(raw_destination)
    source_set = __lsl_qbe_raw_int(raw_source)
    count = __lsl_qbe_load32(destination_set + 0)
    if count != __lsl_qbe_load32(source_set + 0):
        call q_assert(0, q_string_49, q_string_31, 661)
    end
    destination = __lsl_qbe_load64(destination_set + 8)
    source = __lsl_qbe_load64(source_set + 8)
    index = 0
    while index + 4 <= count:
        call __lsl_qbe_store64(destination + (index + 0) * 8, __lsl_qbe_load64(destination + (index + 0) * 8) | __lsl_qbe_load64(source + (index + 0) * 8))
        call __lsl_qbe_store64(destination + (index + 1) * 8, __lsl_qbe_load64(destination + (index + 1) * 8) | __lsl_qbe_load64(source + (index + 1) * 8))
        call __lsl_qbe_store64(destination + (index + 2) * 8, __lsl_qbe_load64(destination + (index + 2) * 8) | __lsl_qbe_load64(source + (index + 2) * 8))
        call __lsl_qbe_store64(destination + (index + 3) * 8, __lsl_qbe_load64(destination + (index + 3) * 8) | __lsl_qbe_load64(source + (index + 3) * 8))
        index += 4
    end
    while index < count:
        call __lsl_qbe_store64(destination + (index + 0) * 8, __lsl_qbe_load64(destination + (index + 0) * 8) | __lsl_qbe_load64(source + (index + 0) * 8))
        index += 1
    end
    return 0
end

function q_f_bsinter(raw_destination, raw_source):
    destination_set = __lsl_qbe_raw_int(raw_destination)
    source_set = __lsl_qbe_raw_int(raw_source)
    count = __lsl_qbe_load32(destination_set + 0)
    if count != __lsl_qbe_load32(source_set + 0):
        call q_assert(0, q_string_49, q_string_31, 662)
    end
    destination = __lsl_qbe_load64(destination_set + 8)
    source = __lsl_qbe_load64(source_set + 8)
    index = 0
    while index + 4 <= count:
        call __lsl_qbe_store64(destination + (index + 0) * 8, __lsl_qbe_load64(destination + (index + 0) * 8) & __lsl_qbe_load64(source + (index + 0) * 8))
        call __lsl_qbe_store64(destination + (index + 1) * 8, __lsl_qbe_load64(destination + (index + 1) * 8) & __lsl_qbe_load64(source + (index + 1) * 8))
        call __lsl_qbe_store64(destination + (index + 2) * 8, __lsl_qbe_load64(destination + (index + 2) * 8) & __lsl_qbe_load64(source + (index + 2) * 8))
        call __lsl_qbe_store64(destination + (index + 3) * 8, __lsl_qbe_load64(destination + (index + 3) * 8) & __lsl_qbe_load64(source + (index + 3) * 8))
        index += 4
    end
    while index < count:
        call __lsl_qbe_store64(destination + (index + 0) * 8, __lsl_qbe_load64(destination + (index + 0) * 8) & __lsl_qbe_load64(source + (index + 0) * 8))
        index += 1
    end
    return 0
end

function q_f_bsdiff(raw_destination, raw_source):
    destination_set = __lsl_qbe_raw_int(raw_destination)
    source_set = __lsl_qbe_raw_int(raw_source)
    count = __lsl_qbe_load32(destination_set + 0)
    if count != __lsl_qbe_load32(source_set + 0):
        call q_assert(0, q_string_49, q_string_31, 663)
    end
    destination = __lsl_qbe_load64(destination_set + 8)
    source = __lsl_qbe_load64(source_set + 8)
    index = 0
    while index + 4 <= count:
        call __lsl_qbe_store64(destination + (index + 0) * 8, __lsl_qbe_load64(destination + (index + 0) * 8) & (-1 ~ __lsl_qbe_load64(source + (index + 0) * 8)))
        call __lsl_qbe_store64(destination + (index + 1) * 8, __lsl_qbe_load64(destination + (index + 1) * 8) & (-1 ~ __lsl_qbe_load64(source + (index + 1) * 8)))
        call __lsl_qbe_store64(destination + (index + 2) * 8, __lsl_qbe_load64(destination + (index + 2) * 8) & (-1 ~ __lsl_qbe_load64(source + (index + 2) * 8)))
        call __lsl_qbe_store64(destination + (index + 3) * 8, __lsl_qbe_load64(destination + (index + 3) * 8) & (-1 ~ __lsl_qbe_load64(source + (index + 3) * 8)))
        index += 4
    end
    while index < count:
        call __lsl_qbe_store64(destination + (index + 0) * 8, __lsl_qbe_load64(destination + (index + 0) * 8) & (-1 ~ __lsl_qbe_load64(source + (index + 0) * 8)))
        index += 1
    end
    return 0
end
