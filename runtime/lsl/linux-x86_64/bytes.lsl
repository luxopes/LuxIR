# lsl: internal-abi
# Byte-buffer helpers. Encoders build short List[Int] values; the assembler,
# the data section and the ELF image accumulate them in ByteArray buffers.
# append_little and patch_little accept either representation.

function append(buffer, value):
    if value < 0 or value > 255:
        Error(ByteRangeError: "byte is outside 0..255")
    end
    buffer.append(value)
end

# Append every element of a List[Int] to a ByteArray. ByteArray.append is a
# typed store and rejects values outside 0..255 itself. Elements arrive as
# boxed or raw Ints depending on the caller's ABI; __lsl_typed_int accepts
# both, unlike the dynamic Int() conversion.
function extend(buffer, values):
    out = ByteArray(buffer)
    for value in values:
        call out.append(__lsl_typed_int(value))
    end
end

# Append the whole content of one ByteArray to another.
function extend_bytes(buffer, values):
    out = ByteArray(buffer)
    source = ByteArray(values)
    count = len(source)
    index = 0
    while index < count:
        call out.append(source[index])
        index += 1
    end
end

function append_little(buffer, value, width):
    # Compiler-internal direct calls carry scalar arguments as boxed Ints.
    # Normalize once into typed raw locals; keeping `remaining` as Any made the
    # byte encoder depend on generic heap classification under peak self-host
    # memory pressure and very rarely emitted another boxed payload.
    remaining = __lsl_typed_int(value)
    byte_width = __lsl_typed_int(width)
    if remaining < 0:
        limit = 1
        for index in rang(byte_width):
            limit *= 256
        end
        remaining += limit
    end
    for index in rang(byte_width):
        buffer.append(remaining % 256)
        remaining = remaining // 256
    end
end

function little(value, width):
    result = []
    call append_little(result, value, width)
    return result
end

function patch_little(buffer, offset, value, width):
    encoded = little(value, width)
    index = 0
    while index < width:
        buffer[offset + index] = __lsl_typed_int(encoded[index])
        index += 1
    end
end

function utf8(text):
    result = []
    for character in text:
        code = ord(character)
        if code <= 127:
            result.append(code)
        elif code <= 2047:
            result.append(192 + code // 64)
            result.append(128 + code % 64)
        elif code <= 65535:
            result.append(224 + code // 4096)
            result.append(128 + (code // 64) % 64)
            result.append(128 + code % 64)
        else:
            result.append(240 + code // 262144)
            result.append(128 + (code // 4096) % 64)
            result.append(128 + (code // 64) % 64)
            result.append(128 + code % 64)
        end
    end
    return result
end

function decimal_spec(buffer):
    parts = []
    for value in buffer:
        parts.append(String(value))
    end
    return ",".join(parts)
end
