# lsl: internal-abi
# Correctly rounded C-locale decimal/hex conversion for QBE's strto/scanf
# surface. Integer limbs keep the input exact until the final ties-to-even
# rounding; binary32 is rounded directly, without an intermediate binary64.

function _digit(raw_byte, raw_radix):
    byte = __lsl_qbe_raw_int(raw_byte)
    radix = __lsl_qbe_raw_int(raw_radix)
    digit = -1
    if byte >= 48 and byte <= 57:
        digit = byte - 48
    elif byte >= 65 and byte <= 70:
        digit = byte - 55
    elif byte >= 97 and byte <= 102:
        digit = byte - 87
    end
    if digit < 0 or digit >= radix:
        return -1
    end
    return digit
end

function _mul(value, raw_factor, raw_carry=0):
    factor = __lsl_qbe_raw_int(raw_factor)
    carry = __lsl_qbe_raw_int(raw_carry)
    for index in rang(len(value)):
        product = __lsl_qbe_raw_int(value[index]) * factor + carry
        value[index] = product & 1073741823
        carry = __lsl_qbe_lshr(product, 30)
    end
    if carry != 0:
        value.append(carry)
    end
end

function _shift(value, raw_bits):
    bits = __lsl_qbe_raw_int(raw_bits)
    result = []
    for unused in rang(bits // 30):
        result.append(0)
    end
    amount = bits % 30
    carry = 0
    for limb in value:
        word = __lsl_qbe_shl(__lsl_qbe_raw_int(limb), amount) | carry
        result.append(word & 1073741823)
        carry = __lsl_qbe_lshr(word, 30)
    end
    if carry != 0:
        result.append(carry)
    end
    return result
end

function _bits(value):
    bits = (len(value) - 1) * 30
    top = __lsl_qbe_raw_int(value[len(value) - 1])
    while top != 0:
        bits += 1
        top = __lsl_qbe_lshr(top, 1)
    end
    return bits
end

function _compare(left, right):
    if len(left) != len(right):
        if len(left) > len(right):
            return 1
        end
        return -1
    end
    index = len(left) - 1
    while index >= 0:
        if __lsl_qbe_raw_int(left[index]) != __lsl_qbe_raw_int(right[index]):
            if __lsl_qbe_raw_int(left[index]) > __lsl_qbe_raw_int(right[index]):
                return 1
            end
            return -1
        end
        index -= 1
    end
    return 0
end

function _subtract(left, right):
    borrow = 0
    for index in rang(len(left)):
        word = __lsl_qbe_raw_int(left[index]) - borrow
        if index < len(right):
            word -= __lsl_qbe_raw_int(right[index])
        end
        borrow = 0
        if word < 0:
            word += 1073741824
            borrow = 1
        end
        left[index] = word
    end
    while len(left) > 1 and left[len(left) - 1] == 0:
        call left.pop()
    end
end

function _round(numerator, denominator, raw_binary_shift, single):
    binary_shift = __lsl_qbe_raw_int(raw_binary_shift)
    precision = 53
    minimum_unit = -1074
    maximum_exponent = 1023
    if single:
        precision = 24
        minimum_unit = -149
        maximum_exponent = 127
    end
    exponent = __lsl_qbe_raw_int(_bits(numerator)) - __lsl_qbe_raw_int(_bits(denominator))
    comparison = 0
    if exponent >= 0:
        comparison = _compare(numerator, _shift(denominator, exponent))
    else:
        comparison = _compare(_shift(numerator, -exponent), denominator)
    end
    if comparison < 0:
        exponent -= 1
    end
    exponent += binary_shift
    if exponent > maximum_exponent:
        return __lsl_float_from_bits(9218868437227405312)
    end
    if exponent < minimum_unit - 1:
        return __lsl_float_from_bits(0)
    end
    unit = __lsl_qbe_raw_int(max(minimum_unit, exponent - precision + 1))
    shift = binary_shift - unit
    if shift >= 0:
        numerator = _shift(numerator, shift)
    else:
        denominator = _shift(denominator, -shift)
    end
    quotient = 0
    bit = __lsl_qbe_raw_int(_bits(numerator)) - __lsl_qbe_raw_int(_bits(denominator))
    while bit >= 0:
        divisor = _shift(denominator, bit)
        if _compare(numerator, divisor) >= 0:
            call _subtract(numerator, divisor)
            quotient |= __lsl_qbe_shl(1, bit)
        end
        bit -= 1
    end
    # Compare the exact remainder with half a unit, including exact midpoints.
    comparison = _compare(_shift(numerator, 1), denominator)
    if comparison > 0 or (comparison == 0 and (quotient & 1) != 0):
        quotient += 1
    end
    if quotient == __lsl_qbe_shl(1, precision):
        quotient = __lsl_qbe_lshr(quotient, 1)
        unit += 1
        exponent += 1
    end
    if exponent > maximum_exponent:
        return __lsl_float_from_bits(9218868437227405312)
    end
    factor_bits = 0
    if unit >= -1022:
        factor_bits = __lsl_qbe_shl(unit + 1023, 52)
    else:
        factor_bits = __lsl_qbe_shl(1, unit + 1074)
    end
    return __lsl_float_from_int(quotient) * __lsl_float_from_bits(factor_bits)
end

function _byte(text, offset):
    if offset >= __lsl_text_byte_length(text):
        return 0
    end
    return __lsl_text_byte(text, offset)
end

function parse(text, single=False):
    index = 0
    while _byte(text, index) in [9, 10, 11, 12, 13, 32]:
        index += 1
    end
    negative = False
    if _byte(text, index) == 43 or _byte(text, index) == 45:
        negative = _byte(text, index) == 45
        index += 1
    end
    lower = text.lower()
    special = 0
    if lower[index:index + 3] == "inf":
        special = 9218868437227405312
        index += 3
        if lower[index:index + 5] == "inity":
            index += 5
        end
    elif lower[index:index + 3] == "nan":
        special = 9221120237041090560
        index += 3
        if _byte(text, index) == 40:
            end_payload = index + 1
            byte = _byte(text, end_payload)
            while (byte >= 48 and byte <= 57) or (byte >= 65 and byte <= 90) or (byte >= 97 and byte <= 122) or byte == 95:
                end_payload += 1
                byte = _byte(text, end_payload)
            end
            if byte == 41:
                index = end_payload + 1
            end
        end
    end
    if special != 0:
        if negative:
            special |= -9223372036854775808
        end
        return [__lsl_float_from_bits(special), index]
    end
    radix = 10
    if _byte(text, index) == 48 and (_byte(text, index + 1) == 120 or _byte(text, index + 1) == 88):
        after = index + 2
        if _byte(text, after) == 46:
            after += 1
        end
        if _digit(_byte(text, after), 16) >= 0:
            radix = 16
            index += 2
        end
    end
    numerator = [0]
    digits = 0
    significant = 0
    places = 0
    point = False
    while True:
        byte = _byte(text, index)
        if byte == 46 and not point:
            point = True
            index += 1
            continue
        end
        digit = _digit(byte, radix)
        if digit < 0:
            break
        end
        call _mul(numerator, radix, digit)
        digits += 1
        if digit != 0 or significant != 0:
            significant += 1
        end
        if point:
            places += 1
        end
        index += 1
    end
    if digits == 0:
        return [__lsl_float_from_bits(0), 0]
    end
    exponent = 0
    marker = _byte(text, index)
    if (radix == 10 and (marker == 101 or marker == 69)) or (radix == 16 and (marker == 112 or marker == 80)):
        marker_index = index
        index += 1
        exponent_negative = False
        if _byte(text, index) == 43 or _byte(text, index) == 45:
            exponent_negative = _byte(text, index) == 45
            index += 1
        end
        exponent_start = index
        byte = _byte(text, index)
        while byte >= 48 and byte <= 57:
            exponent = __lsl_qbe_raw_int(min(1000000, exponent * 10 + byte - 48))
            index += 1
            byte = _byte(text, index)
        end
        if index == exponent_start:
            index = marker_index
        elif exponent_negative:
            exponent = -exponent
        end
    end
    value = __lsl_float_from_bits(0)
    if significant != 0:
        denominator = [1]
        binary_shift = 0
        if radix == 16:
            binary_shift = exponent - 4 * places
            value = _round(numerator, denominator, binary_shift, single)
        else:
            exponent -= places
            order = significant + exponent - 1
            if order > 310:
                value = __lsl_float_from_bits(9218868437227405312)
            elif order >= -326:
                binary_shift = exponent
                if exponent >= 0:
                    for unused in rang(exponent):
                        call _mul(numerator, 5)
                    end
                else:
                    for unused in rang(-exponent):
                        call _mul(denominator, 5)
                    end
                end
                value = _round(numerator, denominator, binary_shift, single)
            end
        end
    end
    if negative:
        value = -value
    end
    return [value, index]
end
