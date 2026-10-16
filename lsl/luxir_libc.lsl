# lsl: internal-abi
# Small libc subset used by QBE 1.3. Addresses and FILE handles follow the
# adjacent qbe_memory/qbe_stdio modules; C scalar arguments are canonicalized
# as native words at the internal ABI, including integer zero.
load luxir_memory as mem
load luxir_stdio as stdio
load luxir_float as fp

function _ctype_byte(value):
    byte = __lsl_qbe_raw_int(value)
    if byte == -1:
        return -1
    end
    if byte < 0 or byte > 255:
        return -1
    end
    return byte
end

function _predicate(value):
    if value:
        return 1
    end
    return 0
end

function c_isalpha(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    upper = Int(byte >= 65) & Int(byte <= 90)
    lower = Int(byte >= 97) & Int(byte <= 122)
    return upper | lower
end

function c_isdigit(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    return Int(byte >= 48) & Int(byte <= 57)
end

function c_isalnum(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    upper = Int(byte >= 65) & Int(byte <= 90)
    lower = Int(byte >= 97) & Int(byte <= 122)
    digit = Int(byte >= 48) & Int(byte <= 57)
    return upper | lower | digit
end

function c_isblank(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    return Int(byte == 32) | Int(byte == 9)
end

function c_isspace(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    space = Int(byte == 32) | Int(byte == 9)
    control_space = Int(byte >= 10) & Int(byte <= 13)
    return space | control_space
end

function c_iscntrl(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    controls = Int(byte >= 0) & Int(byte < 32)
    return controls | Int(byte == 127)
end

function c_isprint(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    return Int(byte >= 32) & Int(byte <= 126)
end

function c_isgraph(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    return Int(byte >= 33) & Int(byte <= 126)
end

function c_islower(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    return Int(byte >= 97) & Int(byte <= 122)
end

function c_isupper(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    return Int(byte >= 65) & Int(byte <= 90)
end

function c_isxdigit(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    digit = Int(byte >= 48) & Int(byte <= 57)
    upper = Int(byte >= 65) & Int(byte <= 70)
    lower = Int(byte >= 97) & Int(byte <= 102)
    return digit | upper | lower
end

function c_ispunct(raw_value):
    byte = __lsl_qbe_raw_int(raw_value)
    first = Int(byte >= 33) & Int(byte <= 47)
    second = Int(byte >= 58) & Int(byte <= 64)
    third = Int(byte >= 91) & Int(byte <= 96)
    fourth = Int(byte >= 123) & Int(byte <= 126)
    return first | second | third | fourth
end

function c_tolower(value):
    byte = _ctype_byte(value)
    if byte >= 65 and byte <= 90:
        return byte + 32
    end
    return byte
end

function c_toupper(value):
    byte = _ctype_byte(value)
    if byte >= 97 and byte <= 122:
        return byte - 32
    end
    return byte
end

function _digit_value(raw_byte):
    byte = __lsl_qbe_raw_int(raw_byte)
    if byte >= 48 and byte <= 57:
        return byte - 48
    end
    if byte >= 65 and byte <= 90:
        return byte - 65 + 10
    end
    if byte >= 97 and byte <= 122:
        return byte - 97 + 10
    end
    return -1
end

function _store_end(raw_end_address, raw_address):
    end_address = __lsl_qbe_raw_int(raw_end_address)
    address = __lsl_qbe_raw_int(raw_address)
    if end_address != 0:
        mem.store(end_address, 8, address)
    end
end

# C integer conversion core. Signed forms saturate at the signed 64-bit
# limits. Unsigned forms preserve the low 64 bits of the parsed magnitude.
function _strto_integer(raw_address, raw_end_address, base, signed):
    address = __lsl_qbe_raw_int(raw_address)
    end_address = __lsl_qbe_raw_int(raw_end_address)
    original = address
    radix = __lsl_qbe_raw_int(base)
    byte = mem.load(address, 1)
    while c_isspace(byte) != 0:
        address += 1
        byte = mem.load(address, 1)
    end
    negative = False
    if byte == 43 or byte == 45:
        negative = byte == 45
        address += 1
        byte = mem.load(address, 1)
    end
    if radix == 0:
        if byte == 48:
            next_byte = mem.load(address + 1, 1)
            if next_byte == 120 or next_byte == 88:
                following = mem.load(address + 2, 1)
                if _digit_value(following) >= 0 and _digit_value(following) < 16:
                    radix = 16
                    address += 2
                else:
                    radix = 8
                end
            elif next_byte == 98 or next_byte == 66:
                following = mem.load(address + 2, 1)
                if following == 48 or following == 49:
                    radix = 2
                    address += 2
                else:
                    radix = 8
                end
            else:
                radix = 8
            end
        else:
            radix = 10
        end
    elif radix == 16 and byte == 48:
        next_byte = mem.load(address + 1, 1)
        following = mem.load(address + 2, 1)
        if (next_byte == 120 or next_byte == 88) and _digit_value(following) >= 0 and _digit_value(following) < 16:
            address += 2
        end
    end
    if radix < 2 or radix > 36:
        _store_end(end_address, original)
        return 0
    end
    digit_start = address
    result = 0
    overflow = False
    signed_limit = 9223372036854775807
    if signed and negative:
        signed_limit = 9223372036854775807
    end
    while True:
        byte = mem.load(address, 1)
        digit = _digit_value(byte)
        if digit < 0 or digit >= radix:
            break
        end
        if signed:
            if result > (signed_limit - digit) // radix:
                overflow = True
            elif not overflow:
                result = result * radix + digit
            end
        else:
            result = result * radix + digit
        end
        address += 1
    end
    if address == digit_start:
        _store_end(end_address, original)
        return 0
    end
    _store_end(end_address, address)
    if signed:
        if overflow:
            if negative:
                return -9223372036854775808
            end
            return 9223372036854775807
        end
        if negative:
            return -result
        end
        return result
    end
    if negative:
        return -result
    end
    return result
end

function c_strtol(address, end_address, base):
    return _strto_integer(address, end_address, base, True)
end

function c_strtoll(address, end_address, base):
    return _strto_integer(address, end_address, base, True)
end

function c_strtoul(address, end_address, base):
    return _strto_integer(address, end_address, base, False)
end

function c_strtoull(address, end_address, base):
    return _strto_integer(address, end_address, base, False)
end

function _float_digit(byte, radix):
    digit = _digit_value(byte)
    if digit >= 0 and digit < radix:
        return digit
    end
    return -1
end

function _pow10(exponent):
    result = 1.0
    if exponent >= 0:
        if exponent > 400:
            return 1.0e308
        end
        for unused in rang(exponent):
            result *= 10.0
        end
    else:
        if exponent < -400:
            return 0.0
        end
        for unused in rang(-exponent):
            result /= 10.0
        end
    end
    return result
end

function _ascii_lower(byte):
    if byte >= 65 and byte <= 90:
        return byte + 32
    end
    return byte
end

function _strto_float(raw_address, raw_end_address, single):
    address = __lsl_qbe_raw_int(raw_address)
    end_address = __lsl_qbe_raw_int(raw_end_address)
    parsed = fp.parse(mem.text(address), single)
    _store_end(end_address, address + __lsl_qbe_raw_int(parsed[1]))
    return parsed[0]
end

function c_strtod(address, end_address):
    return _strto_float(address, end_address, False)
end

function c_strtof(address, end_address):
    return _strto_float(address, end_address, True)
end

function c_abort():
    call stdio.fflush(0)
    exit(134)
end

function c_exit(status):
    call stdio.fflush(0)
    exit(__lsl_qbe_raw_int(status))
end

# Protějšky src/tool.c pro režim -x (spouštění assembleru a linkeru).
function c_luxir_run(raw_argv):
    argv = __lsl_qbe_raw_int(raw_argv)
    program = mem.text(__lsl_qbe_load64(argv))
    arguments = []
    offset = 8
    while __lsl_qbe_load64(argv + offset) != 0:
        arguments.append(mem.text(__lsl_qbe_load64(argv + offset)))
        offset += 8
    end
    call stdio.fflush(0)
    return run(program, arguments)
end

function c_luxir_exists(raw_path):
    if exists(mem.text(__lsl_qbe_raw_int(raw_path))):
        return 1
    end
    return 0
end

function c_luxir_exedir():
    path = executable_path()
    if path == None:
        return 0
    end
    parts = String(path).split("/")
    directory = "/".join(parts[0:len(parts) - 1])
    if directory == "":
        return 0
    end
    return mem.cstr(directory)
end

function c_luxir_unlink(raw_path):
    return unlink(mem.text(__lsl_qbe_raw_int(raw_path)))
end

# Spustitelný soubor: writebytes vytváří soubory s právem spouštění
# (jen nové: existující by si nechal stará práva), proto se soubor
# znovu vytvoří.
function c_luxir_chmod_exe(raw_path):
    path = mem.text(__lsl_qbe_raw_int(raw_path))
    data = readbytes(path)
    call unlink(path)
    call writebytes(path, data)
    return 0
end
