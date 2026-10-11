# lsl: internal-abi
# C stdio surface used by QBE 1.3. Handles 1..3 are the standard streams;
# other handles are small records kept in the module's private table.
load luxir_memory as mem
load luxir_stdio_raw as raw
load luxir_float as fp

_streams = {1: {"kind": "in", "fd": 0, "data": "", "byte_length": 0, "position": 0, "eof": False, "unread": None},
            2: {"kind": "out", "fd": 1,
                "direct_buffer": __lsl_byte_zeros(8192), "direct_base": 0, "direct_used": 0},
            3: {"kind": "out", "fd": 2,
                "direct_buffer": __lsl_byte_zeros(8192), "direct_base": 0, "direct_used": 0}}
_streams[2]["direct_base"] = __lsl_qbe_load64(__lsl_qbe_raw_int(_streams[2]["direct_buffer"]) + 24)
_streams[3]["direct_base"] = __lsl_qbe_load64(__lsl_qbe_raw_int(_streams[3]["direct_buffer"]) + 24)
_output_cache = __lsl_int_zeros(4)
_output_cache_base = __lsl_qbe_load64(__lsl_qbe_raw_int(_output_cache) + 24)
_output_cache_root = [None]
__lsl_qbe_store64(_output_cache_base, -1)
_state = {"next_stream": 4}
# Numeric cache fields are words so the byte-at-a-time getc path stays native.
# Keep the String separately as a strong root for its backing bytes.
_input_cache = __lsl_int_zeros(7)
_input_cache_base = __lsl_qbe_load64(__lsl_qbe_raw_int(_input_cache) + 24)
_input_cache_data = [""]
__lsl_qbe_store64(_input_cache_base + 40, -1)

function _char(raw_address, raw_offset):
    address = __lsl_qbe_raw_int(raw_address)
    offset = __lsl_qbe_raw_int(raw_offset)
    value = mem.load(address + offset, 1)
    if value == 0:
        return None
    end
    return chr(value)
end

function _cstr(raw_address):
    address = __lsl_qbe_raw_int(raw_address)
    return mem.text(address)
end

function _sync_input_cache():
    active = __lsl_qbe_load64(_input_cache_base)
    if active != 0 and active in _streams:
        stream = _streams[active]
        stream["position"] = __lsl_qbe_load64(_input_cache_base + 32)
        stream["eof"] = __lsl_qbe_load64(_input_cache_base + 48) != 0
        unread = __lsl_qbe_load64(_input_cache_base + 40)
        if unread < 0:
            stream["unread"] = None
        else:
            stream["unread"] = unread
        end
    end
    return 0
end

function _activate_input(raw_file):
    file = __lsl_qbe_raw_int(raw_file)
    if file == __lsl_qbe_load64(_input_cache_base):
        return 1
    end
    call _sync_input_cache()
    if file not in _streams:
        return 0
    end
    stream = _streams[file]
    if stream["kind"] != "in":
        return 0
    end
    __lsl_qbe_store64(_input_cache_base, file)
    __lsl_qbe_store64(_input_cache_base + 8, 0)
    if file == 1:
        __lsl_qbe_store64(_input_cache_base + 8, stream["fd"])
    end
    data = stream["data"]
    _input_cache_data[0] = data
    __lsl_qbe_store64(_input_cache_base + 16, __lsl_address(data) + 16)
    __lsl_qbe_store64(_input_cache_base + 24, stream["byte_length"])
    __lsl_qbe_store64(_input_cache_base + 32, stream["position"])
    __lsl_qbe_store64(_input_cache_base + 40, -1)
    if stream["unread"] != None:
        __lsl_qbe_store64(_input_cache_base + 40, stream["unread"])
    end
    __lsl_qbe_store64(_input_cache_base + 48, 0)
    if stream["eof"]:
        __lsl_qbe_store64(_input_cache_base + 48, 1)
    end
    return 1
end

function _digit_string(number, base, upper=False):
    radix = __lsl_qbe_raw_int(base)
    digits = "0123456789abcdef"
    if upper:
        digits = "0123456789ABCDEF"
    end
    n = __lsl_typed_int(number)
    if n == 0:
        return "0"
    end
    result = ""
    while n > 0:
        result = digits[n % radix] + result
        n = n // radix
    end
    return result
end

function _unsigned_string(number, base, upper=False):
    number_value = __lsl_qbe_raw_int(number)
    radix = __lsl_qbe_raw_int(base)
    if number_value >= 0:
        return _digit_string(number_value, radix, upper)
    end
    digits = "0123456789abcdef"
    if upper:
        digits = "0123456789ABCDEF"
    end
    if radix == 10:
        magnitude = ""
        if number_value == -9223372036854775808:
            magnitude = "9223372036854775808"
        else:
            magnitude = _digit_string(-(number_value + 1), 10)
            # magnitude above is |number|-1; add one in decimal text.
            carry = 1
            i = len(magnitude) - 1
            while i >= 0 and carry:
                digit = ord(magnitude[i]) - ord("0") + carry
                if digit >= 10:
                    magnitude = magnitude[:i] + "0" + magnitude[i + 1:]
                    carry = 1
                else:
                    magnitude = magnitude[:i] + chr(ord("0") + digit) + magnitude[i + 1:]
                    carry = 0
                end
                i -= 1
            end
            if carry:
                magnitude = "1" + magnitude
            end
        end
        minuend = "18446744073709551616"
        result = ""
        borrow = 0
        left = len(minuend) - 1
        right = len(magnitude) - 1
        while left >= 0:
            a = ord(minuend[left]) - ord("0")
            b = 0
            if right >= 0:
                b = ord(magnitude[right]) - ord("0")
            end
            digit = a - b - borrow
            if digit < 0:
                digit += 10
                borrow = 1
            else:
                borrow = 0
            end
            result = chr(ord("0") + digit) + result
            left -= 1
            right -= 1
        end
        while len(result) > 1 and result[0] == "0":
            result = result[1:]
        end
        return result
    end
    # For power-of-two bases, complement the magnitude-1 bit pattern to
    # obtain the low 64 bits of the unsigned value.
    bits_per_digit = 4
    if radix == 8:
        bits_per_digit = 3
    end
    bit_text = ""
    magnitude = -(number_value + 1)
    for unused in rang(64):
        bit_text = digits[1 - (magnitude % 2)] + bit_text
        magnitude = magnitude // 2
    end
    padding = (bits_per_digit - (len(bit_text) % bits_per_digit)) % bits_per_digit
    bit_text = ("0" * padding) + bit_text
    result = ""
    offset = 0
    while offset < len(bit_text):
        value = 0
        for bit in rang(bits_per_digit):
            value = value * 2 + ord(bit_text[offset + bit]) - ord("0")
        end
        result += digits[value]
        offset += bits_per_digit
    end
    while len(result) > 1 and result[0] == "0":
        result = result[1:]
    end
    return result
end

function _pad(text, raw_width, left=False, zero=False):
    width = __lsl_qbe_raw_int(raw_width)
    amount = width - __lsl_text_byte_length(text)
    if amount <= 0:
        return text
    end
    fill = " "
    if zero:
        fill = "0"
    end
    padding = fill * amount
    if left:
        return text + padding
    end
    return padding + text
end

function _decimal_mul_small(digits, raw_factor):
    factor = __lsl_qbe_raw_int(raw_factor)
    carry = 0
    result = ""
    for index in rang(len(digits)):
        reverse_index = len(digits) - 1 - index
        value = (ord(digits[reverse_index]) - ord("0")) * factor + carry
        result = chr(ord("0") + value % 10) + result
        carry = value // 10
    end
    while carry > 0:
        result = chr(ord("0") + carry % 10) + result
        carry = carry // 10
    end
    return result
end

function _round_decimal_integer(digits, raw_cut):
    cut = __lsl_qbe_raw_int(raw_cut)
    # Round digits / 10^cut to an integer, exactly, ties to even.
    position = len(digits) - cut
    if position <= 0:
        kept = "0"
        discarded = ("0" * (0 - position)) + digits
    elif position >= len(digits):
        return digits + ("0" * (position - len(digits)))
    else:
        kept = digits[:position]
        discarded = digits[position:]
    end
    first = 0
    if len(discarded) > 0:
        first = ord(discarded[0]) - ord("0")
    end
    more = False
    for index in rang(1, len(discarded)):
        if discarded[index] != "0":
            more = True
        end
    end
    last = ord(kept[len(kept)-1]) - ord("0")
    if first > 5 or (first == 5 and (more or last % 2 == 1)):
        kept = _decimal_mul_small(kept, 1)
        carry = 1
        result = ""
        for index in rang(len(kept)):
            reverse_index = len(kept) - 1 - index
            digit = ord(kept[reverse_index]) - ord("0") + carry
            result = chr(ord("0") + digit % 10) + result
            carry = digit // 10
        end
        if carry > 0:
            result = "1" + result
        end
        kept = result
    end
    return kept
end

function _fixed_float(value, raw_precision, uppercase=False):
    precision = __lsl_qbe_raw_int(raw_precision)
    places = precision
    if places < 0:
        places = 6
    end
    if places > 18:
        Error(ValueError: "printf float precision above 18 is unsupported")
    end
    # Varargs already carry promoted LSL Float values. Avoid re-running the
    # dynamic Float constructor here: generated C ABI wrappers may preserve a
    # typed float object that the constructor rejects as a generic Any.
    bits = __lsl_float_bits(value)
    negative = bits < 0
    exponent = __lsl_qbe_lshr(bits, 52) & 2047
    fraction = bits & 4503599627370495
    sign = ""
    if negative:
        sign = "-"
    end
    if exponent == 2047:
        special = "inf"
        if fraction != 0:
            special = "nan"
        end
        if uppercase:
            special = special.upper()
        end
        return sign + special
    end
    if exponent == 0:
        significand = fraction
        power = -1074
    else:
        significand = fraction + 4503599627370496
        power = exponent - 1075
    end
    if significand == 0:
        result = "0"
        if places > 0:
            result += "." + ("0" * places)
        end
        return sign + result
    end

    digits = _digit_string(significand, 10)
    if power >= 0:
        for unused in rang(power):
            digits = _decimal_mul_small(digits, 2)
        end
        scaled = digits + ("0" * places)
    else:
        for unused in rang(0 - power):
            digits = _decimal_mul_small(digits, 5)
        end
        scaled = _round_decimal_integer(digits, (0 - power) - places)
    end
    if places > 0:
        while len(scaled) <= places:
            scaled = "0" + scaled
        end
        split = len(scaled) - places
        result = scaled[:split] + "." + scaled[split:]
    else:
        result = scaled
    end
    return sign + result
end

function _pad_number(text, raw_width, left, zero):
    width = __lsl_qbe_raw_int(raw_width)
    amount = width - len(text)
    if amount <= 0:
        return text
    end
    if left:
        return text + (" " * amount)
    end
    if zero and len(text) > 0 and (text[0] == "-" or text[0] == "+" or text[0] == " "):
        return text[0] + ("0" * amount) + text[1:]
    end
    if zero:
        return ("0" * amount) + text
    end
    return (" " * amount) + text
end

# Format a C format string with the QBE varargs convention: each promoted
# scalar occupies one List element, and string conversions contain addresses.
function _format_uncached(raw_format_address, arguments):
    format_address = __lsl_qbe_raw_int(raw_format_address)
    format = _cstr(format_address)
    pieces = []
    argument = 0
    i = 0
    format_bytes = __lsl_text_byte_length(format)
    literal_start = 0
    while i < format_bytes:
        byte = __lsl_text_byte(format, i)
        if byte != 37:
            i += 1
            continue
        end
        if i > literal_start:
            pieces.append(__lsl_text_piece(format, literal_start, i - literal_start))
        end
        i += 1
        if i < format_bytes and __lsl_text_byte(format, i) == 37:
            pieces.append("%")
            i += 1
            literal_start = i
            continue
        end
        left = False
        zero = False
        plus = False
        space = False
        alternate = False
        while i < format_bytes:
            byte = __lsl_text_byte(format, i)
            if byte != 45 and byte != 43 and byte != 32 and byte != 35 and byte != 48:
                break
            end
            if byte == 45:
                left = True
            end
            if byte == 48:
                zero = True
            end
            if byte == 43:
                plus = True
            end
            if byte == 32:
                space = True
            end
            if byte == 35:
                alternate = True
            end
            i += 1
        end
        width = 0
        if i < format_bytes and __lsl_text_byte(format, i) == 42:
            width = __lsl_qbe_raw_int(arguments[argument])
            argument += 1
            i += 1
            if width < 0:
                width = -width
                left = True
            end
        else:
            while i < format_bytes:
                byte = __lsl_text_byte(format, i)
                if byte < 48 or byte > 57:
                    break
                end
                width = width * 10 + byte - 48
                i += 1
            end
        end
        precision = -1
        if i < format_bytes and __lsl_text_byte(format, i) == 46:
            i += 1
            precision = 0
            if i < format_bytes and __lsl_text_byte(format, i) == 42:
                precision = __lsl_qbe_raw_int(arguments[argument])
                argument += 1
                i += 1
            else:
                while i < format_bytes:
                    byte = __lsl_text_byte(format, i)
                    if byte < 48 or byte > 57:
                        break
                    end
                    precision = precision * 10 + byte - 48
                    i += 1
                end
            end
        end
        # Length modifiers do not alter QBE's promoted argument representation.
        wide = False
        while i < format_bytes:
            byte = __lsl_text_byte(format, i)
            if byte != 104 and byte != 108 and byte != 122 and byte != 116 and byte != 106 and byte != 76:
                break
            end
            if byte == 108 or byte == 106 or byte == 122 or byte == 116:
                wide = True
            end
            i += 1
        end
        if i >= format_bytes:
            break
        end
        conversion = chr(__lsl_text_byte(format, i))
        i += 1
        literal_start = i
        if conversion == "n":
            argument += 1
            continue
        end
        base = 10
        if conversion == "o":
            base = 8
        end
        if conversion == "x" or conversion == "X" or conversion == "p":
            base = 16
        end
        if argument >= len(arguments):
            value = 0
        else:
            value = arguments[argument]
        end
        argument += 1
        text = ""
        if conversion == "s":
            text = _cstr(__lsl_qbe_raw_int(value))
            if precision >= 0 and precision < __lsl_text_byte_length(text):
                text = __lsl_text_piece(text, 0, precision)
            end
            pieces.append(_pad(text, width, left, False))
        elif conversion == "c":
            char_text = chr(__lsl_qbe_raw_int(value) % 256)
            pieces.append(char_text)
        elif conversion == "d" or conversion == "i":
            number = __lsl_qbe_raw_int(value)
            if number < 0:
                if number == -9223372036854775808:
                    text = "-9223372036854775808"
                else:
                    text = "-" + _digit_string(-number, 10)
                end
            else:
                text = _digit_string(number, 10)
                if plus:
                    text = "+" + text
                elif space:
                    text = " " + text
                end
            end
            digits_start = 0
            if len(text) > 0 and (text[0] == "-" or text[0] == "+" or text[0] == " "):
                digits_start = 1
            end
            if precision == 0 and number == 0:
                text = text[:digits_start]
            end
            if precision > len(text) - digits_start:
                text = text[:digits_start] + ("0" * (precision - (len(text) - digits_start))) + text[digits_start:]
            end
            pieces.append(_pad_number(text, width, left, zero and precision < 0))
        elif conversion == "u" or conversion == "o" or conversion == "x" or conversion == "X" or conversion == "p":
            number = __lsl_qbe_raw_int(value)
            if number < 0:
                if wide or conversion == "p":
                    text = _unsigned_string(number, base, conversion == "X")
                else:
                    number = number & 4294967295
                end
            end
            if number >= 0 or not wide and conversion != "p":
                text = _digit_string(number, base, conversion == "X")
            end
            if precision == 0 and number == 0:
                text = ""
            end
            if precision > len(text):
                text = ("0" * (precision-len(text))) + text
            end
            if alternate and conversion == "o" and (len(text) == 0 or text[0] != "0"):
                text = "0" + text
            end
            if (alternate or conversion == "p") and (conversion == "x" or conversion == "X" or conversion == "p") and number != 0:
                if conversion == "X":
                    text = "0X" + text
                else:
                    text = "0x" + text
                end
            end
            pieces.append(_pad_number(text, width, left, zero and precision < 0))
        elif conversion == "f" or conversion == "F":
            # QBE's emitter uses %f in assembly comments; keep the libc default
            # of six fractional digits and honor an explicit precision.
            text = _fixed_float(value, precision)
            if (len(text) == 0 or text[0] != "-") and plus:
                text = "+" + text
            elif (len(text) == 0 or text[0] != "-") and space:
                text = " " + text
            end
            if conversion == "F":
                text = text.upper()
            end
            pieces.append(_pad_number(text, width, left, zero))
        else:
            pieces.append("%" + conversion)
        end
    end
    if format_bytes > literal_start:
        pieces.append(__lsl_text_piece(format, literal_start, format_bytes - literal_start))
    end
    return "".join(pieces)
end

# Plans contain literal spans and parsed directives. They are cached only for
# addresses explicitly registered by qbe_memory.literal(); caller-owned C
# strings keep the uncached path so writes/free/reuse remain observable.
_format_plans = {}
_raw_format_plans = {}
_word_format_plans = {}
_word_plan_cache = __lsl_int_zeros(1024)
_word_plan_cache_base = __lsl_qbe_load64(__lsl_qbe_raw_int(_word_plan_cache) + 24)

function _parse_format_plan(format):
    plan = []
    i = 0
    format_bytes = __lsl_text_byte_length(format)
    literal_start = 0
    while i < format_bytes:
        if __lsl_text_byte(format, i) != 37:
            i += 1
            continue
        end
        if i > literal_start:
            plan.append([0, __lsl_text_piece(format, literal_start, i - literal_start)])
        end
        i += 1
        if i < format_bytes and __lsl_text_byte(format, i) == 37:
            plan.append([0, "%"])
            i += 1
            literal_start = i
            continue
        end
        left = False
        zero = False
        plus = False
        space = False
        alternate = False
        while i < format_bytes:
            byte = __lsl_text_byte(format, i)
            if byte != 45 and byte != 43 and byte != 32 and byte != 35 and byte != 48:
                break
            end
            if byte == 45:
                left = True
            end
            if byte == 48:
                zero = True
            end
            if byte == 43:
                plus = True
            end
            if byte == 32:
                space = True
            end
            if byte == 35:
                alternate = True
            end
            i += 1
        end
        width = 0
        width_star = False
        if i < format_bytes and __lsl_text_byte(format, i) == 42:
            width_star = True
            i += 1
        else:
            while i < format_bytes:
                byte = __lsl_text_byte(format, i)
                if byte < 48 or byte > 57:
                    break
                end
                width = width * 10 + byte - 48
                i += 1
            end
        end
        precision = -1
        precision_star = False
        if i < format_bytes and __lsl_text_byte(format, i) == 46:
            i += 1
            precision = 0
            if i < format_bytes and __lsl_text_byte(format, i) == 42:
                precision_star = True
                i += 1
            else:
                while i < format_bytes:
                    byte = __lsl_text_byte(format, i)
                    if byte < 48 or byte > 57:
                        break
                    end
                    precision = precision * 10 + byte - 48
                    i += 1
                end
            end
        end
        wide = False
        while i < format_bytes:
            byte = __lsl_text_byte(format, i)
            if byte != 104 and byte != 108 and byte != 122 and byte != 116 and byte != 106 and byte != 76:
                break
            end
            if byte == 108 or byte == 106 or byte == 122 or byte == 116:
                wide = True
            end
            i += 1
        end
        if i >= format_bytes:
            break
        end
        conversion = chr(__lsl_text_byte(format, i))
        i += 1
        literal_start = i
        plan.append([1, conversion, left, zero, plus, space, alternate,
            width, width_star, precision, precision_star, wide])
    end
    if format_bytes > literal_start:
        plan.append([0, __lsl_text_piece(format, literal_start, format_bytes - literal_start)])
    end
    return plan
end

function _format_c_prefix(format):
    byte_count = __lsl_text_byte_length(format)
    length = 0
    while length < byte_count and __lsl_text_byte(format, length) != 0:
        length += 1
    end
    if length == byte_count:
        return format
    end
    return __lsl_text_piece(format, 0, length)
end

function _render_format_directive(token, arguments, raw_argument):
    argument = __lsl_qbe_raw_int(raw_argument)
    conversion = token[1]
    left = token[2]
    zero = token[3]
    plus = token[4]
    space = token[5]
    alternate = token[6]
    width = token[7]
    if token[8]:
        width = __lsl_qbe_raw_int(arguments[argument])
        argument += 1
        if width < 0:
            width = -width
            left = True
        end
    end
    precision = token[9]
    if token[10]:
        precision = __lsl_qbe_raw_int(arguments[argument])
        argument += 1
    end
    wide = token[11]
    if conversion == "n":
        return ["", argument + 1]
    end
    if argument >= len(arguments):
        value = 0
    else:
        value = arguments[argument]
    end
    argument += 1
    text = ""
    if conversion == "s":
        text = _cstr(__lsl_qbe_raw_int(value))
        if precision >= 0 and precision < __lsl_text_byte_length(text):
            text = __lsl_text_piece(text, 0, precision)
        end
        text = _pad(text, width, left, False)
    elif conversion == "c":
        text = chr(__lsl_qbe_raw_int(value) % 256)
    elif conversion == "d" or conversion == "i":
        number = __lsl_qbe_raw_int(value)
        if number < 0:
            if number == -9223372036854775808:
                text = "-9223372036854775808"
            else:
                text = "-" + _digit_string(-number, 10)
            end
        else:
            text = _digit_string(number, 10)
            if plus:
                text = "+" + text
            elif space:
                text = " " + text
            end
        end
        digits_start = 0
        if len(text) > 0 and (text[0] == "-" or text[0] == "+" or text[0] == " "):
            digits_start = 1
        end
        if precision == 0 and number == 0:
            text = text[:digits_start]
        end
        if precision > len(text) - digits_start:
            text = text[:digits_start] + ("0" * (precision - (len(text) - digits_start))) + text[digits_start:]
        end
        text = _pad_number(text, width, left, zero and precision < 0)
    elif conversion == "u" or conversion == "o" or conversion == "x" or conversion == "X" or conversion == "p":
        base = 10
        if conversion == "o":
            base = 8
        end
        if conversion == "x" or conversion == "X" or conversion == "p":
            base = 16
        end
        number = __lsl_qbe_raw_int(value)
        if number < 0:
            if wide or conversion == "p":
                text = _unsigned_string(number, base, conversion == "X")
            else:
                number = number & 4294967295
            end
        end
        if number >= 0 or not wide and conversion != "p":
            text = _digit_string(number, base, conversion == "X")
        end
        if precision == 0 and number == 0:
            text = ""
        end
        if precision > len(text):
            text = ("0" * (precision-len(text))) + text
        end
        if alternate and conversion == "o" and (len(text) == 0 or text[0] != "0"):
            text = "0" + text
        end
        if (alternate or conversion == "p") and (conversion == "x" or conversion == "X" or conversion == "p") and number != 0:
            if conversion == "X":
                text = "0X" + text
            else:
                text = "0x" + text
            end
        end
        text = _pad_number(text, width, left, zero and precision < 0)
    elif conversion == "f" or conversion == "F":
        text = _fixed_float(value, precision)
        if (len(text) == 0 or text[0] != "-") and plus:
            text = "+" + text
        elif (len(text) == 0 or text[0] != "-") and space:
            text = " " + text
        end
        if conversion == "F":
            text = text.upper()
        end
        text = _pad_number(text, width, left, zero)
    else:
        text = "%" + conversion
    end
    return [text, argument]
end

function _render_format_plan(plan, arguments):
    pieces = []
    argument = 0
    for token in plan:
        if token[0] == 0:
            pieces.append(token[1])
        else:
            rendered = _render_format_directive(token, arguments, argument)
            pieces.append(rendered[0])
            argument = rendered[1]
        end
    end
    return "".join(pieces)
end

function _format(raw_format_address, arguments):
    format_address = __lsl_qbe_raw_int(raw_format_address)
    # A plan key can only be installed after literal_text() registered the
    # address. Literal allocations are immutable and never reused by malloc.
    if format_address in _format_plans:
        return _render_format_plan(_format_plans[format_address], arguments)
    end
    literal = mem.literal_text(format_address)
    if literal == None:
        return _format_uncached(format_address, arguments)
    end
    parsed = _parse_format_plan(_format_c_prefix(literal))
    _format_plans[format_address] = parsed
    if _raw_plan_eligible(parsed):
        _raw_format_plans[format_address] = parsed
        packed = raw.pack_word_plan(parsed)
        _word_format_plans[format_address] = packed
        cache_slot = _word_plan_cache_base + ((format_address >> 4) & 511) * 16
        __lsl_qbe_store64(cache_slot, format_address)
        __lsl_qbe_store64(cache_slot + 8, __lsl_qbe_load64(__lsl_qbe_raw_int(packed) + 24))
    end
    return _render_format_plan(parsed, arguments)
end

function _raw_plan_eligible(plan):
    for token in plan:
        if token[0] == 0:
            continue
        end
        conversion = token[1]
        if conversion != "s" and conversion != "d" and conversion != "i" and conversion != "u" and conversion != "x" and conversion != "X":
            return False
        end
        if token[6]:
            return False
        end
    end
    return True
end

function _select_output(file):
    cache = _output_cache_base
    cached_file = __lsl_qbe_load64(cache)
    if cached_file != file:
        if cached_file >= 1 and cached_file in _streams:
            _streams[cached_file]["direct_used"] = __lsl_qbe_load64(cache + 24)
        end
        if file not in _streams or _streams[file]["kind"] != "out":
            return None
        end
        stream = _streams[file]
        buffer = stream["direct_buffer"]
        base = stream["direct_base"]
        __lsl_qbe_store64(cache, file)
        __lsl_qbe_store64(cache + 8, stream["fd"])
        __lsl_qbe_store64(cache + 16, base)
        __lsl_qbe_store64(cache + 24, stream["direct_used"])
        _output_cache_root[0] = buffer
        return stream
    end
    if file not in _streams or _streams[file]["kind"] != "out":
        return None
    end
    return _streams[file]
end

function _flush(raw_file):
    file = __lsl_qbe_raw_int(raw_file)
    if file == 1:
        return 0
    end
    if file not in _streams:
        return -1
    end
    if _streams[file]["kind"] != "out":
        return 0
    end
    stream = _select_output(file)
    if stream == None:
        return -1
    end
    cache = _output_cache_base
    used = __lsl_qbe_load64(cache + 24)
    if used == 0:
        return 0
    end
    write_status = __lsl_fd_write(__lsl_qbe_load64(cache + 8),
        _output_cache_root[0], used)
    if write_status < 0:
        return -1
    end
    __lsl_qbe_store64(cache + 24, 0)
    stream["direct_used"] = 0
    return 0
end

function _append_pointer(raw_file, raw_pointer, raw_count):
    file = __lsl_qbe_raw_int(raw_file)
    pointer = __lsl_qbe_raw_int(raw_pointer)
    count = __lsl_qbe_raw_int(raw_count)
    if count < 0 or file < 1:
        return -1
    end
    cache = _output_cache_base
    if __lsl_qbe_load64(cache) != file:
        if _select_output(file) == None:
            return -1
        end
    end
    used = __lsl_qbe_load64(cache + 24)
    base = __lsl_qbe_load64(cache + 16)
    source = pointer
    remaining = count
    while remaining > 0:
        if used >= 8192:
            __lsl_qbe_store64(cache + 24, used)
            if _flush(file) < 0:
                return -1
            end
            used = 0
        end
        amount = min(remaining, 8192 - used)
        for index in rang(amount):
            __lsl_qbe_store8(base + used + index,
                __lsl_qbe_load8(source + index))
        end
        used += amount
        source += amount
        remaining -= amount
    end
    __lsl_qbe_store64(cache + 24, used)
    if file == 3 or count >= 8192:
        if _flush(file) < 0:
            return -1
        end
    end
    return count
end

function _append_byte_array(raw_file, raw_bytes, raw_count):
    file = __lsl_qbe_raw_int(raw_file)
    bytes = raw_bytes
    count = __lsl_qbe_raw_int(raw_count)
    if count < 0:
        return -1
    end
    stream = _select_output(file)
    if stream == None:
        return -1
    end
    cache = _output_cache_base
    used = __lsl_qbe_load64(cache + 24)
    destination = _output_cache_root[0]
    remaining = count
    source_offset = 0
    while remaining > 0:
        if used >= 8192:
            __lsl_qbe_store64(cache + 24, used)
            stream["direct_used"] = used
            if _flush(file) < 0:
                return -1
            end
            used = 0
        end
        amount = min(remaining, 8192 - used)
        call __lsl_byte_copy(destination, bytes, used, source_offset, amount)
        used += amount
        source_offset += amount
        remaining -= amount
    end
    __lsl_qbe_store64(cache + 24, used)
    stream["direct_used"] = used
    if file == 3 or count >= 8192:
        if _flush(file) < 0:
            return -1
        end
    end
    return count
end

function fprintf(raw_file, raw_format, arguments):
    file = __lsl_qbe_raw_int(raw_file)
    format = __lsl_qbe_raw_int(raw_format)
    if file < 1 or file not in _streams:
        return -1
    end
    if _streams[file]["kind"] != "out":
        return -1
    end
    if (file == 2 or file >= 4) and format in _raw_format_plans:
        stream = _select_output(file)
        cache = _output_cache_base
        used = __lsl_qbe_load64(cache + 24)
        next_used = raw.format_plan_render_into(_raw_format_plans[format], arguments,
            _output_cache_root[0], used)
        if next_used < 0 and used > 0:
            # The measurement pass guaranteed no bytes were written. Empty the
            # stream buffer and retry before selecting the allocation fallback.
            if _flush(file) < 0:
                return -1
            end
            stream = _select_output(file)
            used = 0
            next_used = raw.format_plan_render_into(_raw_format_plans[format], arguments,
                _output_cache_root[0], 0)
        end
        if next_used >= 0:
            amount = next_used - used
            __lsl_qbe_store64(cache + 24, next_used)
            stream["direct_used"] = next_used
            if file == 3 or next_used == 8192:
                if _flush(file) < 0:
                    return -1
                end
            end
            return amount
        end
        raw_output = raw.format_plan_bytes(_raw_format_plans[format], arguments)
        if raw_output != None:
            return _append_byte_array(file, raw_output[0], raw_output[1])
        end
    end
    text = _format(format, arguments)
    output_bytes = __lsl_text_byte_length(text)
    return _append_pointer(file, __lsl_qbe_raw_int(text) + 16, output_bytes)
end

# Translated C variadic calls with integer/pointer arguments use a caller-owned
# native word array. The allocation-free path is deliberately limited to
# cached raw plans; complete C formatting remains available through the
# original List-based API for mutable or unsupported formats.
function fprintf_words(raw_file, raw_format, raw_arguments, raw_count):
    file = __lsl_qbe_raw_int(raw_file)
    format = __lsl_qbe_raw_int(raw_format)
    arguments = __lsl_qbe_raw_int(raw_arguments)
    count = __lsl_qbe_raw_int(raw_count)
    if file < 1 or count < 0:
        return -1
    end
    cache = _output_cache_base
    if __lsl_qbe_load64(cache) != file:
        if _select_output(file) == None:
            return -1
        end
    end
    if file != 3:
        slot = _word_plan_cache_base + ((format >> 4) & 511) * 16
        plan = 0
        if __lsl_qbe_load64(slot) == format:
            plan = __lsl_qbe_load64(slot + 8)
        elif format in _word_format_plans:
            packed = _word_format_plans[format]
            plan = __lsl_qbe_load64(__lsl_qbe_raw_int(packed) + 24)
            __lsl_qbe_store64(slot, format)
            __lsl_qbe_store64(slot + 8, plan)
        end
        if plan != 0:
            used = __lsl_qbe_load64(cache + 24)
            base = __lsl_qbe_load64(cache + 16)
            next_used = raw.format_words_packed(plan, used, arguments, count, base, 8192)
            if next_used < 0 and used > 0:
                if _flush(file) < 0:
                    return -1
                end
                used = 0
                next_used = raw.format_words_packed(plan, 0, arguments, count, base, 8192)
            end
            if next_used >= 0:
                amount = next_used - used
                # The cache owns the live count until a stream switch/flush,
                # exactly as for fputc. Avoid a dictionary update per call.
                __lsl_qbe_store64(cache + 24, next_used)
                if next_used == 8192:
                    if _flush(file) < 0:
                        return -1
                    end
                end
                return amount
            end
        end
    end
    fallback = []
    for index in rang(count):
        fallback.append(__lsl_qbe_load64(arguments + index * 8))
    end
    return fprintf(file, format, fallback)
end

function printf_words(raw_format, raw_arguments, raw_count):
    return fprintf_words(2, raw_format, raw_arguments, raw_count)
end

function vfprintf(file, format, arguments):
    return fprintf(file, format, arguments)
end

function printf(format, arguments):
    return fprintf(2, format, arguments)
end

function snprintf(raw_destination, raw_size, raw_format, arguments):
    destination = __lsl_qbe_raw_int(raw_destination)
    size = __lsl_qbe_raw_int(raw_size)
    format = __lsl_qbe_raw_int(raw_format)
    if format in _raw_format_plans:
        plan = _raw_format_plans[format]
        required = raw.format_plan_render_into(plan, arguments, 0, 0,
            0, -1, 0, -1, True)
        if required >= 0:
            if size == 0:
                return required
            end
            if size > 0 and required < size:
                written = raw.format_plan_render_into(plan, arguments, 0, 0,
                    0, -1, destination, size - 1, False)
                if written == required:
                    mem.store(destination + written, 1, 0)
                    return required
                end
            end
        end
    end
    text = _format(format, arguments)
    byte_count = __lsl_text_byte_length(text)
    requested = size
    capacity = max(0, requested - 1)
    amount = min(byte_count, capacity)
    for i in rang(amount):
        mem.store(destination + i, 1, __lsl_text_byte(text, i))
    end
    if requested > 0:
        mem.store(destination + amount, 1, 0)
    end
    return byte_count
end

function vsnprintf(destination, size, format, arguments):
    return snprintf(destination, size, format, arguments)
end

# Word-buffer counterpart for translated integer/pointer-only C varargs. It
# shares the raw-plan measurement and direct C-buffer renderer with snprintf;
# a managed List is built only when the full compatibility path is needed.
function snprintf_words(raw_destination, raw_size, raw_format,
        raw_arguments, raw_count):
    destination = __lsl_qbe_raw_int(raw_destination)
    size = __lsl_qbe_raw_int(raw_size)
    format = __lsl_qbe_raw_int(raw_format)
    arguments = __lsl_qbe_raw_int(raw_arguments)
    count = __lsl_qbe_raw_int(raw_count)
    if count < 0:
        return -1
    end
    if format in _raw_format_plans:
        plan = _raw_format_plans[format]
        required = raw.format_plan_render_into(plan, None, 0, 0,
            arguments, count, 0, -1, True)
        if required >= 0:
            if size == 0:
                return required
            end
            if size > 0 and required < size:
                written = raw.format_plan_render_into(plan, None, 0, 0,
                    arguments, count, destination, size - 1, False)
                if written == required:
                    mem.store(destination + written, 1, 0)
                    return required
                end
            end
        end
    end
    fallback = []
    for index in rang(count):
        fallback.append(__lsl_qbe_load64(arguments + index * 8))
    end
    return snprintf(destination, size, format, fallback)
end

function vsnprintf_words(destination, size, format, arguments, count):
    return snprintf_words(destination, size, format, arguments, count)
end

function sprintf(raw_destination, raw_format, arguments):
    destination = __lsl_qbe_raw_int(raw_destination)
    format = __lsl_qbe_raw_int(raw_format)
    text = _format(format, arguments)
    byte_count = __lsl_text_byte_length(text)
    for i in rang(byte_count):
        mem.store(destination + i, 1, __lsl_text_byte(text, i))
    end
    mem.store(destination + byte_count, 1, 0)
    return byte_count
end

function fputs(text, file):
    pointer = __lsl_qbe_raw_int(text)
    return _append_pointer(file, pointer, mem.strlen(pointer))
end

function puts(text):
    result = fputs(text, 2)
    call fprintf(2, mem.cstr("%s"), [mem.cstr(chr(10))])
    return result
end

function fputc(raw_character, raw_file):
    character = __lsl_qbe_raw_int(raw_character)
    file = __lsl_qbe_raw_int(raw_file)
    cache = _output_cache_base
    if file < 1:
        return -1
    end
    if __lsl_qbe_load64(cache) != file:
        stream = _select_output(file)
        if stream == None:
            return -1
        end
    end
    if __lsl_qbe_load64(cache) != file:
        return -1
    end
    used = __lsl_qbe_load64(cache + 24)
    if used >= 8192:
        _streams[file]["direct_used"] = used
        if _flush(file) < 0:
            return -1
        end
        used = 0
    end
    __lsl_qbe_store8(__lsl_qbe_load64(cache + 16) + used, character & 255)
    used += 1
    __lsl_qbe_store64(cache + 24, used)
    if file == 3 or used == 8192:
        _streams[file]["direct_used"] = used
        if _flush(file) < 0:
            return -1
        end
    end
    return character % 256
end

function putchar(character):
    return fputc(character, 2)
end

function fopen(raw_path_address, raw_mode_address):
    path_address = __lsl_qbe_raw_int(raw_path_address)
    mode_address = __lsl_qbe_raw_int(raw_mode_address)
    path = _cstr(path_address)
    mode = _cstr(mode_address)
    if path == "" or mode == "":
        return 0
    end
    if mode[0] != "r" and mode[0] != "w" and mode[0] != "a":
        return 0
    end
    descriptor = 0
    if mode[0] == "r":
        try:
            contents = readfile(path)
        else:
            return 0
        end
        kind = "in"
    else:
        contents = ""
        kind = "out"
        flags = 577
        if mode[0] == "a":
            flags = 1089
        end
        descriptor = __lsl_fd_open(path, flags)
        if descriptor < 0:
            return 0
        end
    end
    handle = _state["next_stream"]
    _state["next_stream"] += 1
    _streams[handle] = {"kind": kind, "path": path, "data": contents,
        "byte_length": __lsl_text_byte_length(contents),
        "position": 0,
        "direct_buffer": __lsl_byte_zeros(8192), "direct_base": 0, "direct_used": 0,
        "fd": descriptor, "eof": False,
        "unread": None}
    _streams[handle]["direct_base"] = __lsl_qbe_load64(
        __lsl_qbe_raw_int(_streams[handle]["direct_buffer"]) + 24)
    return handle
end

function _scan_digit(byte, radix):
    if byte >= 48 and byte <= 57:
        digit = byte - 48
    elif byte >= 65 and byte <= 90:
        digit = byte - 65 + 10
    elif byte >= 97 and byte <= 122:
        digit = byte - 97 + 10
    else:
        return -1
    end
    if digit < radix:
        return digit
    end
    return -1
end

function _scan_float_value(token, single=False):
    parsed = fp.parse(token, single)
    return [parsed[0], __lsl_qbe_raw_int(parsed[1]) != 0]
end

# The upstream parser's only input scans are "_%f" and "_%lf". Keep this
# deliberately narrow and parse those C numeric tokens directly.
function fscanf(raw_file, raw_format_address, arguments):
    file = __lsl_qbe_raw_int(raw_file)
    format_address = __lsl_qbe_raw_int(raw_format_address)
    format = _cstr(format_address)
    if format != "_%f" and format != "_%lf":
        return 0
    end
    first = getc(file)
    if first < 0:
        return -1
    end
    if first != ord("_"):
        call ungetc(first, file)
        return 0
    end
    token = ""
    byte = getc(file)
    while byte == 32 or byte == 9 or byte == 10 or byte == 11 or byte == 12 or byte == 13:
        byte = getc(file)
    end
    if byte < 0:
        return -1
    end
    while byte >= 0:
        char = chr(byte)
        if (byte >= 48 and byte <= 57) or (byte >= 65 and byte <= 90) or (byte >= 97 and byte <= 122) or byte == 43 or byte == 45 or byte == 46 or byte == 95:
            token += char
            byte = getc(file)
        else:
            break
        end
    end
    special_token = token.lower()
    if (special_token == "nan" or special_token == "+nan" or special_token == "-nan") and byte == ord("("):
        token += chr(byte)
        byte = getc(file)
        while byte >= 0 and byte != ord(")"):
            token += chr(byte)
            byte = getc(file)
        end
        if byte == ord(")"):
            token += chr(byte)
            byte = getc(file)
        end
    end
    if byte >= 0:
        call ungetc(byte, file)
    end
    parsed = _scan_float_value(token, format == "_%f")
    if not parsed[1]:
        return 0
    end
    destination = __lsl_typed_int(arguments[0])
    single = format == "_%f"
    mem.store_float(destination, parsed[0], single)
    return 1
end

# Pure cache probe for translated C readers. -2 requests the ordinary getc
# path for activation or stdin I/O; bytes and EOF retain their C values.
# Keeping this leaf separate avoids managed allocation/error checks for every
# byte of an already buffered input. The String remains rooted by the cache.
function getc_ready(raw_file):
    file = __lsl_qbe_raw_int(raw_file)
    cache = _input_cache_base
    if file < 1 or file != __lsl_qbe_load64(cache):
        return -2
    end
    unread = __lsl_qbe_load64(cache + 40)
    if unread >= 0:
        __lsl_qbe_store64(cache + 40, -1)
        return unread
    end
    if file == 1:
        return -2
    end
    position = __lsl_qbe_load64(cache + 32)
    if position >= __lsl_qbe_load64(cache + 24):
        __lsl_qbe_store64(cache + 48, 1)
        return -1
    end
    value = __lsl_qbe_load8(__lsl_qbe_load64(cache + 16) + position)
    __lsl_qbe_store64(cache + 32, position + 1)
    return value
end

function getc(raw_file):
    file = __lsl_qbe_raw_int(raw_file)
    if not _activate_input(file):
        return -1
    end
    if __lsl_qbe_load64(_input_cache_base + 40) >= 0:
        value = __lsl_qbe_load64(_input_cache_base + 40)
        __lsl_qbe_store64(_input_cache_base + 40, -1)
        return value
    end
    if file == 1:
        bytes = __lsl_byte_zeros(1)
        count = __lsl_fd_read(__lsl_qbe_load64(_input_cache_base + 8), bytes, 1)
        if count <= 0:
            __lsl_qbe_store64(_input_cache_base + 48, 1)
            return -1
        end
        return bytes[0]
    end
    position = __lsl_qbe_load64(_input_cache_base + 32)
    if position >= __lsl_qbe_load64(_input_cache_base + 24):
        __lsl_qbe_store64(_input_cache_base + 48, 1)
        return -1
    end
    value = __lsl_qbe_load8(__lsl_qbe_load64(_input_cache_base + 16) + position)
    __lsl_qbe_store64(_input_cache_base + 32, position + 1)
    return value
end

function fgetc(file):
    return getc(file)
end

function ungetc(raw_character, raw_file):
    file = __lsl_qbe_raw_int(raw_file)
    if not _activate_input(file):
        return -1
    end
    byte = __lsl_qbe_raw_int(raw_character)
    if byte == -1:
        return -1
    end
    value = byte % 256
    __lsl_qbe_store64(_input_cache_base + 40, value)
    __lsl_qbe_store64(_input_cache_base + 48, 0)
    return value
end

# fread pro čtečku LuxIR: zkopíruje bajty z načteného souboru najednou;
# stdin se čte po blocích přímo z deskriptoru.
function fread(raw_destination, raw_size, raw_count, raw_file):
    destination = __lsl_qbe_raw_int(raw_destination)
    size = __lsl_qbe_raw_int(raw_size)
    count = __lsl_qbe_raw_int(raw_count)
    file = __lsl_qbe_raw_int(raw_file)
    if size <= 0 or count <= 0:
        return 0
    end
    if not _activate_input(file):
        return 0
    end
    wanted = size * count
    done = 0
    unread = __lsl_qbe_load64(_input_cache_base + 40)
    if unread >= 0:
        call __lsl_qbe_store8(destination, unread)
        __lsl_qbe_store64(_input_cache_base + 40, -1)
        done = 1
    end
    if file == 1:
        while done < wanted:
            chunk = wanted - done
            if chunk > 65536:
                chunk = 65536
            end
            bytes = __lsl_byte_zeros(chunk)
            got = __lsl_fd_read(__lsl_qbe_load64(_input_cache_base + 8), bytes, chunk)
            if got <= 0:
                __lsl_qbe_store64(_input_cache_base + 48, 1)
                break
            end
            call mem.memcpy(destination + done, __lsl_qbe_load64(__lsl_qbe_raw_int(bytes) + 24), got)
            done += got
        end
        return done // size
    end
    position = __lsl_qbe_load64(_input_cache_base + 32)
    available = __lsl_qbe_load64(_input_cache_base + 24) - position
    chunk = wanted - done
    if chunk > available:
        chunk = available
    end
    if chunk > 0:
        call mem.memcpy(destination + done, __lsl_qbe_load64(_input_cache_base + 16) + position, chunk)
        __lsl_qbe_store64(_input_cache_base + 32, position + chunk)
        done += chunk
    end
    if done < wanted:
        __lsl_qbe_store64(_input_cache_base + 48, 1)
    end
    return done // size
end

function feof(raw_file):
    file = __lsl_qbe_raw_int(raw_file)
    if not _activate_input(file):
        return 1
    end
    if __lsl_qbe_load64(_input_cache_base + 48) != 0:
        return 1
    end
    return 0
end

function fflush(raw_file):
    file = __lsl_qbe_raw_int(raw_file)
    if file == 0:
        for handle in _streams:
            call _flush(handle)
        end
        return 0
    end
    if file not in _streams:
        return -1
    end
    return _flush(file)
end

function fclose(raw_file):
    file = __lsl_qbe_raw_int(raw_file)
    if file < 4 or file not in _streams:
        return -1
    end
    if file == __lsl_qbe_load64(_input_cache_base):
        call _sync_input_cache()
        __lsl_qbe_store64(_input_cache_base, 0)
        _input_cache_data[0] = ""
        __lsl_qbe_store64(_input_cache_base + 16, 0)
        __lsl_qbe_store64(_input_cache_base + 24, 0)
        __lsl_qbe_store64(_input_cache_base + 32, 0)
        __lsl_qbe_store64(_input_cache_base + 40, -1)
        __lsl_qbe_store64(_input_cache_base + 48, 0)
    end
    flush_status = _flush(file)
    close_status = 0
    if _streams[file]["kind"] == "out":
        close_status = __lsl_fd_close(_streams[file]["fd"])
    end
    if __lsl_qbe_load64(_output_cache_base) == file:
        __lsl_qbe_store64(_output_cache_base, -1)
        __lsl_qbe_store64(_output_cache_base + 8, 0)
        __lsl_qbe_store64(_output_cache_base + 16, 0)
        __lsl_qbe_store64(_output_cache_base + 24, 0)
        _output_cache_root[0] = None
    end
    del(_streams[file])
    if flush_status < 0 or close_status < 0:
        return -1
    end
    return 0
end
