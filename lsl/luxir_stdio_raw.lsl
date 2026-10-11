# lsl: internal-abi
# Byte-oriented fast renderer for immutable printf plans. Unsupported
# conversions fall back to qbe_stdio's complete String renderer.
# Fast conversions are byte-safe strings and decimal/hex integers with full
# width and precision handling.
load luxir_memory as mem

function _digits(number, base):
    value = __lsl_qbe_raw_int(number)
    radix = __lsl_qbe_raw_int(base)
    count = 1
    while value >= radix:
        value = __lsl_qbe_udiv(value, radix)
        count += 1
    end
    return count
end

function _write_text(raw_base, raw_offset, text, raw_start, raw_count):
    base = __lsl_qbe_raw_int(raw_base)
    offset = __lsl_qbe_raw_int(raw_offset)
    start = __lsl_qbe_raw_int(raw_start)
    count = __lsl_qbe_raw_int(raw_count)
    source = __lsl_qbe_raw_int(text) + 16 + start
    for index in rang(count):
        __lsl_qbe_store8(base + offset + index, __lsl_qbe_load8(source + index))
    end
    return offset + count
end

function _write_pointer(raw_base, raw_offset, pointer, raw_count):
    base = __lsl_qbe_raw_int(raw_base)
    offset = __lsl_qbe_raw_int(raw_offset)
    address = __lsl_qbe_raw_int(pointer)
    count = __lsl_qbe_raw_int(raw_count)
    for index in rang(count):
        __lsl_qbe_store8(base + offset + index, __lsl_qbe_load8(address + index))
    end
    return offset + count
end

function _write_integer(raw_base, raw_offset, number, base, digits, sign, raw_width,
        raw_precision, left, zero, uppercase):
    destination = __lsl_qbe_raw_int(raw_base)
    offset = __lsl_qbe_raw_int(raw_offset)
    value = __lsl_qbe_raw_int(number)
    radix = __lsl_qbe_raw_int(base)
    digit_count = __lsl_qbe_raw_int(digits)
    width = __lsl_qbe_raw_int(raw_width)
    precision = __lsl_qbe_raw_int(raw_precision)
    sign_byte = __lsl_qbe_raw_int(sign)
    minimum = digit_count
    if precision > minimum:
        minimum = precision
    end
    if digit_count == 0:
        minimum = max(0, precision)
    end
    total = minimum
    if sign_byte != 0:
        total += 1
    end
    padding = max(0, width - total)
    leading_spaces = 0
    leading_zeroes = max(0, minimum - digit_count)
    trailing_spaces = 0
    if left:
        trailing_spaces = padding
    elif zero and precision < 0:
        leading_zeroes += padding
    else:
        leading_spaces = padding
    end
    if offset < 0 or total < 0 or padding < 0 or offset + total + padding > 16777216:
        return -1
    end
    for unused in rang(leading_spaces):
        __lsl_qbe_store8(destination + offset, 32)
        offset += 1
    end
    if sign_byte != 0:
        __lsl_qbe_store8(destination + offset, sign_byte)
        offset += 1
    end
    for unused in rang(leading_zeroes):
        __lsl_qbe_store8(destination + offset, 48)
        offset += 1
    end
    if digit_count > 0:
        # The output range was sized in the planning pass. Write digits in
        # reverse directly into their final positions, avoiding a scratch
        # ByteArray and a second copy.
        position = offset + digit_count - 1
        remaining = value
        while position >= offset:
            digit = __lsl_qbe_urem(remaining, radix)
            byte = digit + 48
            if digit >= 10:
                if uppercase:
                    byte = digit + 55
                else:
                    byte = digit + 87
                end
            end
            __lsl_qbe_store8(destination + position, byte)
            remaining = __lsl_qbe_udiv(remaining, radix)
            position -= 1
        end
        offset += digit_count
    end
    for unused in rang(trailing_spaces):
        __lsl_qbe_store8(destination + offset, 32)
        offset += 1
    end
    return offset
end

# Pack an eligible immutable plan once. The caller keeps both this IntArray
# and the original plan (which owns the literal Strings) rooted. Each record
# stores opcode, flags/pointer, width/length, precision, and three argument
# indexes. This avoids managed List access and String comparisons per byte
# of assembly emitted by translated fprintf calls.
function pack_word_plan(plan):
    packed = __lsl_int_zeros(1 + 7 * len(plan))
    address = __lsl_qbe_load64(__lsl_qbe_raw_int(packed) + 24)
    __lsl_qbe_store64(address, len(plan))
    record = address + 8
    argument = 0
    for token in plan:
        if token[0] == 0:
            __lsl_qbe_store64(record, 0)
            __lsl_qbe_store64(record + 8, __lsl_qbe_raw_int(token[1]) + 16)
            __lsl_qbe_store64(record + 16, __lsl_text_byte_length(token[1]))
        else:
            flags = 0
            if token[2]:
                flags = flags | 1
            end
            if token[3]:
                flags = flags | 2
            end
            if token[4]:
                flags = flags | 4
            end
            if token[5]:
                flags = flags | 8
            end
            if token[11]:
                flags = flags | 16
            end
            __lsl_qbe_store64(record, __lsl_text_byte(token[1], 0))
            __lsl_qbe_store64(record + 8, flags)
            __lsl_qbe_store64(record + 16, __lsl_qbe_raw_int(token[7]))
            __lsl_qbe_store64(record + 24, __lsl_qbe_raw_int(token[9]))
            __lsl_qbe_store64(record + 32, -1)
            __lsl_qbe_store64(record + 40, -1)
            if token[8]:
                __lsl_qbe_store64(record + 32, argument)
                argument += 1
            end
            if token[10]:
                __lsl_qbe_store64(record + 40, argument)
                argument += 1
            end
            __lsl_qbe_store64(record + 48, argument)
            argument += 1
        end
        record += 56
    end
    return packed
end

function _word_argument(raw_arguments, raw_count, raw_index):
    arguments = __lsl_qbe_raw_int(raw_arguments)
    count = __lsl_qbe_raw_int(raw_count)
    index = __lsl_qbe_raw_int(raw_index)
    if index >= count:
        return 0
    end
    return __lsl_qbe_load64(arguments + index * 8)
end

# Only for a private stream buffer. On failure its unused tail may have been
# overwritten, but the live prefix and committed byte count are unchanged.
# The caller can therefore flush the prefix, retry, or use the full renderer.
# Every token is bounds-checked before writing; no scratch allocation or
# repeated measurement pass is needed.
function format_words_packed(raw_plan, raw_offset, raw_arguments, raw_count,
        raw_base, raw_capacity):
    plan = __lsl_qbe_raw_int(raw_plan)
    offset = __lsl_qbe_raw_int(raw_offset)
    arguments = __lsl_qbe_raw_int(raw_arguments)
    count = __lsl_qbe_raw_int(raw_count)
    base = __lsl_qbe_raw_int(raw_base)
    capacity = __lsl_qbe_raw_int(raw_capacity)
    if offset < 0 or offset > capacity:
        return -1
    end
    record = plan + 8
    limit = record + __lsl_qbe_load64(plan) * 56
    while record < limit:
        conversion = __lsl_qbe_load64(record)
        if conversion == 0:
            length = __lsl_qbe_load64(record + 16)
            if length > capacity - offset:
                return -1
            end
            offset = _write_pointer(base, offset, __lsl_qbe_load64(record + 8), length)
            record += 56
            continue
        end
        flags = __lsl_qbe_load64(record + 8)
        width = __lsl_qbe_load64(record + 16)
        precision = __lsl_qbe_load64(record + 24)
        width_index = __lsl_qbe_load64(record + 32)
        precision_index = __lsl_qbe_load64(record + 40)
        if width_index >= 0:
            width = _word_argument(arguments, count, width_index)
            if width < 0:
                width = -width
                flags = flags | 1
            end
        end
        if precision_index >= 0:
            precision = _word_argument(arguments, count, precision_index)
        end
        if width < 0 or width > capacity - offset:
            return -1
        end
        value = _word_argument(arguments, count, __lsl_qbe_load64(record + 48))
        if conversion == 115:
            length = mem.strlen(value)
            if precision >= 0 and precision < length:
                length = precision
            end
            if length > capacity - offset:
                return -1
            end
            padding = max(0, width - length)
            if (flags & 1) == 0:
                for unused in rang(padding):
                    __lsl_qbe_store8(base + offset, 32)
                    offset += 1
                end
            end
            offset = _write_pointer(base, offset, value, length)
            if flags & 1:
                for unused in rang(padding):
                    __lsl_qbe_store8(base + offset, 32)
                    offset += 1
                end
            end
        else:
            radix = 10
            if conversion == 120 or conversion == 88:
                radix = 16
            end
            sign = 0
            if conversion == 100 or conversion == 105:
                if value < 0:
                    if value == -9223372036854775808:
                        return -1
                    end
                    sign = 45
                    value = -value
                elif flags & 4:
                    sign = 43
                elif flags & 8:
                    sign = 32
                end
            elif value < 0:
                if flags & 16:
                    return -1
                end
                value = value & 4294967295
            end
            digits = _digits(value, radix)
            if precision == 0 and value == 0:
                digits = 0
            end
            length = max(digits, precision)
            if sign != 0:
                length += 1
            end
            if length > capacity - offset:
                return -1
            end
            offset = _write_integer(base, offset, value, radix, digits, sign,
                width, precision, flags & 1, flags & 2, conversion == 88)
        end
        record += 56
    end
    return offset
end

# Render an immutable printf plan directly into an existing stream ByteArray.
# The measurement pass is read-only: a capacity miss returns -1 without
# touching the destination, so the caller can flush and retry safely.
function _format_argument(arguments, raw_argv, raw_argc, index):
    argv = __lsl_qbe_raw_int(raw_argv)
    argc = __lsl_qbe_raw_int(raw_argc)
    argument_index = __lsl_qbe_raw_int(index)
    if argc >= 0:
        if argument_index >= argc:
            return 0
        end
        return __lsl_qbe_load64(argv + argument_index * 8)
    end
    if argument_index >= len(arguments):
        return 0
    end
    return arguments[argument_index]
end

function format_plan_render_into(plan, arguments, destination, raw_offset,
        raw_argv=0, raw_argc=-1, raw_output_base=0,
        raw_output_capacity=-1, measure_only=False):
    argv = __lsl_qbe_raw_int(raw_argv)
    argc = __lsl_qbe_raw_int(raw_argc)
    output_base = __lsl_qbe_raw_int(raw_output_base)
    output_capacity = __lsl_qbe_raw_int(raw_output_capacity)
    offset = __lsl_qbe_raw_int(raw_offset)
    limit = 8192
    if output_capacity >= 0:
        limit = output_capacity
    elif measure_only:
        limit = 16777216
    end
    if offset < 0 or offset > limit:
        return -1
    end
    required = 0
    argument = 0
    for token in plan:
        token_kind = __lsl_qbe_raw_int(token[0])
        if token_kind == 0:
            required += __lsl_text_byte_length(token[1])
            continue
        end
        conversion = token[1]
        if conversion != "s" and conversion != "d" and conversion != "i" and conversion != "u" and conversion != "x" and conversion != "X":
            return -1
        end
        alternate = __lsl_qbe_raw_int(token[6])
        if alternate:
            return -1
        end
        width = __lsl_qbe_raw_int(token[7])
        width_star = __lsl_qbe_raw_int(token[8])
        if width_star:
            width = __lsl_qbe_raw_int(_format_argument(arguments, argv, argc, argument))
            argument += 1
            if width < 0:
                width = -width
            end
        end
        precision = __lsl_qbe_raw_int(token[9])
        precision_star = __lsl_qbe_raw_int(token[10])
        if precision_star:
            precision = __lsl_qbe_raw_int(_format_argument(arguments, argv, argc, argument))
            argument += 1
        end
        value = _format_argument(arguments, argv, argc, argument)
        argument += 1
        if conversion == "s":
            pointer = __lsl_qbe_raw_int(value)
            count = mem.strlen(pointer)
            if precision >= 0 and precision < count:
                count = precision
            end
            required += count + max(0, width - count)
        else:
            number = __lsl_qbe_raw_int(value)
            radix = 10
            if conversion == "x" or conversion == "X":
                radix = 16
            end
            sign = 0
            if conversion == "d" or conversion == "i":
                if number < 0:
                    if number == -9223372036854775808:
                        return -1
                    end
                    sign = 45
                    number = -number
                elif __lsl_qbe_raw_int(token[4]):
                    sign = 43
                elif __lsl_qbe_raw_int(token[5]):
                    sign = 32
                end
            elif number < 0:
                if __lsl_qbe_raw_int(token[11]):
                    return -1
                end
                number = number & 4294967295
            end
            digits = _digits(number, radix)
            if precision == 0 and number == 0:
                digits = 0
            end
            minimum = max(digits, precision)
            if minimum < 0:
                minimum = digits
            end
            body = minimum
            if sign != 0:
                body += 1
            end
            required += body + max(0, width - body)
        end
        if required > limit - offset:
            return -1
        end
    end
    if required > limit - offset:
        return -1
    end
    if measure_only:
        return required
    end
    base_address = output_base
    if output_capacity < 0:
        raw_destination = __lsl_qbe_raw_int(destination)
        base_address = __lsl_qbe_load64(raw_destination + 24)
    end
    if base_address == 0 and required > 0:
        return -1
    end

    # The second pass repeats argument resolution, now with a proven complete
    # destination range. No operations array or temporary ByteArray is built.
    argument = 0
    for token in plan:
        token_kind = __lsl_qbe_raw_int(token[0])
        if token_kind == 0:
            text = token[1]
            count = __lsl_text_byte_length(text)
            offset = _write_text(base_address, offset, text, 0, count)
            continue
        end
        conversion = token[1]
        width = __lsl_qbe_raw_int(token[7])
        left = __lsl_qbe_raw_int(token[2])
        if __lsl_qbe_raw_int(token[8]):
            width = __lsl_qbe_raw_int(_format_argument(arguments, argv, argc, argument))
            argument += 1
            if width < 0:
                width = -width
                left = True
            end
        end
        precision = __lsl_qbe_raw_int(token[9])
        if __lsl_qbe_raw_int(token[10]):
            precision = __lsl_qbe_raw_int(_format_argument(arguments, argv, argc, argument))
            argument += 1
        end
        value = _format_argument(arguments, argv, argc, argument)
        argument += 1
        if conversion == "s":
            pointer = __lsl_qbe_raw_int(value)
            count = mem.strlen(pointer)
            if precision >= 0 and precision < count:
                count = precision
            end
            padding = max(0, width - count)
            if not left:
                for unused in rang(padding):
                    __lsl_qbe_store8(base_address + offset, 32)
                    offset += 1
                end
            end
            offset = _write_pointer(base_address, offset, pointer, count)
            if left:
                for unused in rang(padding):
                    __lsl_qbe_store8(base_address + offset, 32)
                    offset += 1
                end
            end
        else:
            number = __lsl_qbe_raw_int(value)
            radix = 10
            if conversion == "x" or conversion == "X":
                radix = 16
            end
            sign = 0
            if conversion == "d" or conversion == "i":
                if number < 0:
                    sign = 45
                    number = -number
                elif __lsl_qbe_raw_int(token[4]):
                    sign = 43
                elif __lsl_qbe_raw_int(token[5]):
                    sign = 32
                end
            elif number < 0:
                number = number & 4294967295
            end
            digits = _digits(number, radix)
            if precision == 0 and number == 0:
                digits = 0
            end
            offset = _write_integer(base_address, offset, number, radix, digits,
                sign, width, precision, left, __lsl_qbe_raw_int(token[3]), conversion == "X")
        end
    end
    return offset
end

# Return [ByteArray, byte_count], or None if a directive needs the complete
# compatibility renderer. Supported conversions are byte-safe strings,
# characters and decimal/octal/hex integer formats with full width/precision.
function format_plan_bytes(plan, arguments):
    operations = []
    argument = 0
    output_size = 0
    for token in plan:
        token_kind = __lsl_qbe_raw_int(token[0])
        if token_kind == 0:
            text = token[1]
            count = __lsl_text_byte_length(text)
            operations.append([0, text, count])
            output_size += count
            continue
        end
        conversion = token[1]
        if conversion != "s" and conversion != "d" and conversion != "i" and conversion != "u" and conversion != "x" and conversion != "X":
            return None
        end
        if __lsl_qbe_raw_int(token[6]):
            return None
        end
        width = __lsl_qbe_raw_int(token[7])
        left = __lsl_qbe_raw_int(token[2])
        if __lsl_qbe_raw_int(token[8]):
            width = __lsl_qbe_raw_int(arguments[argument])
            argument += 1
            if width < 0:
                width = -width
                left = True
            end
        end
        precision = __lsl_qbe_raw_int(token[9])
        if __lsl_qbe_raw_int(token[10]):
            precision = __lsl_qbe_raw_int(arguments[argument])
            argument += 1
        end
        if argument >= len(arguments):
            value = 0
        else:
            value = arguments[argument]
        end
        argument += 1
        if conversion == "s":
            pointer = __lsl_qbe_raw_int(value)
            count = mem.strlen(pointer)
            if precision >= 0 and precision < count:
                count = precision
            end
            padding = max(0, width - count)
            operations.append([1, pointer, count, padding, left])
            output_size += count + padding
        else:
            number = __lsl_qbe_raw_int(value)
            base = 10
            if conversion == "x" or conversion == "X":
                base = 16
            end
            sign = 0
            if conversion == "d" or conversion == "i":
                if number < 0:
                    if number == -9223372036854775808:
                        return None
                    end
                    sign = 45
                    number = -number
                elif __lsl_qbe_raw_int(token[4]):
                    sign = 43
                elif __lsl_qbe_raw_int(token[5]):
                    sign = 32
                end
            else:
                if number < 0:
                    wide = __lsl_qbe_raw_int(token[11])
                    if not wide:
                        number = number & 4294967295
                    else:
                        return None
                    end
                end
            end
            digits = _digits(number, base)
            if precision == 0 and number == 0:
                digits = 0
            end
            minimum = max(digits, precision)
            if minimum < 0:
                minimum = digits
            end
            body_size = minimum
            if sign != 0:
                body_size += 1
            end
            padding = max(0, width - body_size)
            output_size += body_size + padding
            operations.append([3, number, base, digits, sign, width, precision,
                left, __lsl_qbe_raw_int(token[3]), conversion == "X"])
        end
        if output_size > 16777216:
            return None
        end
    end
    bytes = __lsl_byte_zeros(output_size)
    base = __lsl_qbe_load64(__lsl_qbe_raw_int(bytes) + 24)
    if output_size > 0 and base == 0:
        return None
    end
    offset = 0
    for operation in operations:
        operation_size = 0
        if operation[0] == 0:
            operation_size = operation[2]
            if offset < 0 or operation_size < 0 or offset + operation_size > output_size:
                return None
            end
            offset = _write_text(base, offset, operation[1], 0, operation_size)
        elif operation[0] == 1:
            pointer = operation[1]
            count = operation[2]
            padding = operation[3]
            left = operation[4]
            operation_size = count + padding
            if offset < 0 or count < 0 or padding < 0 or offset + operation_size > output_size:
                return None
            end
            if not left:
                for unused in rang(padding):
                    __lsl_qbe_store8(base + offset, 32)
                    offset += 1
                end
            end
            offset = _write_pointer(base, offset, pointer, count)
            if left:
                for unused in rang(padding):
                    __lsl_qbe_store8(base + offset, 32)
                    offset += 1
                end
            end
        else:
            precision = operation[6]
            digit_count = operation[3]
            minimum = max(digit_count, precision)
            if digit_count == 0:
                minimum = max(0, precision)
            elif minimum < 0:
                minimum = digit_count
            end
            operation_size = minimum
            if operation[4] != 0:
                operation_size += 1
            end
            operation_size += max(0, operation[5] - operation_size)
            if offset < 0 or operation_size < 0 or offset + operation_size > output_size:
                return None
            end
            next_offset = _write_integer(base, offset, operation[1], operation[2],
                operation[3], operation[4], operation[5], operation[6],
                operation[7], operation[8], operation[9])
            if next_offset < 0:
                return None
            end
            offset = next_offset
        end
    end
    return [bytes, offset]
end
