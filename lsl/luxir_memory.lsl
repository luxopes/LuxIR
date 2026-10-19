# lsl: internal-abi
# C-owned storage for the host. Small allocations use reusable size-class
# slabs; large ones are mapped individually. On Linux (any architecture:
# syscall numbers come from the target's _platform module) the memory comes
# from mmap/munmap; on Windows and macOS from zeroed ByteArrays pinned in
# _pinned (the collector does not move objects, so addresses stay valid).
# Raw allocations stay outside the managed object graph. No libc/FFI calls.
load _platform as platform
_pinned = {}
_literals = {}
_literal_texts = None
_literal_list = []
_free_heads = __lsl_int_zeros(12)
_slab_next = __lsl_int_zeros(12)
_slab_end = __lsl_int_zeros(12)
# The fixed IntArrays remain global roots; their 12-slot backing buffers never
# resize. Size-class indices are 0..11, so these private accesses need no dynamic
# receiver/type checks. IntArray payloads use the shared collection ABI at +24.
_free_heads_data = __lsl_qbe_load64(__lsl_qbe_raw_int(_free_heads) + 24)
_slab_next_data = __lsl_qbe_load64(__lsl_qbe_raw_int(_slab_next) + 24)
_slab_end_data = __lsl_qbe_load64(__lsl_qbe_raw_int(_slab_end) + 24)
# QBE's PFn allocations all expire at freeall. Keep one chunk chain and a
# bump cursor for that lifetime, separately from individually freed objects.
_function_arena = __lsl_int_zeros(4)
_function_arena_data = __lsl_qbe_load64(__lsl_qbe_raw_int(_function_arena) + 24)

# Zero-filled raw memory of at least size bytes, aligned to 16 bytes; negative
# on failure.
function raw_map(size):
    if platform.NAME.startswith("linux-"):
        return __lsl_syscall6(platform.SYS_MMAP, 0, size, 3, 34, -1, 0)
    end
    bytes = __lsl_byte_zeros(size + 16)
    base = (__lsl_qbe_raw_int(__lsl_data_address(bytes)) + 15) & -16
    _pinned[base] = bytes
    return base
end

# Releases raw_map memory; 0 on success.
function raw_unmap(base, size):
    if platform.NAME.startswith("linux-"):
        return __lsl_syscall6(platform.SYS_MUNMAP, base, size, 0, 0, 0, 0)
    end
    if base not in _pinned:
        return -1
    end
    call _pinned.pop(base)
    return 0
end

function function_alloc(raw_size):
    size = __lsl_qbe_raw_int(raw_size)
    if size == 0:
        return 0
    end
    if size < 0 or size > 1099511627776:
        Error(MemoryError: "QBE function allocation exceeds host limit")
    end
    aligned = (size + 15) & -16
    cursor = __lsl_qbe_load64(_function_arena_data + 8)
    limit = __lsl_qbe_load64(_function_arena_data + 16)
    if cursor + aligned > limit:
        previous = __lsl_qbe_load64(_function_arena_data + 24)
        chunk = __lsl_qbe_load64(_function_arena_data)
        if previous != 0:
            chunk = __lsl_qbe_load64(previous)
        end
        while chunk != 0:
            if __lsl_qbe_load64(chunk + 8) - 16 >= aligned:
                break
            end
            previous = chunk
            chunk = __lsl_qbe_load64(chunk)
        end
        if chunk == 0:
            mapped = max(1048576, (aligned + 16 + 4095) & -4096)
            chunk = raw_map(mapped)
            if chunk < 0:
                Error(MemoryError: "QBE function arena allocation failed")
            end
            call __lsl_qbe_store64(chunk, 0)
            call __lsl_qbe_store64(chunk + 8, mapped)
            if previous == 0:
                call __lsl_qbe_store64(_function_arena_data, chunk)
            else:
                call __lsl_qbe_store64(previous, chunk)
            end
        end
        cursor = chunk + 16
        call __lsl_qbe_store64(_function_arena_data + 16, chunk + __lsl_qbe_load64(chunk + 8))
        call __lsl_qbe_store64(_function_arena_data + 24, chunk)
    end
    call __lsl_qbe_store64(_function_arena_data + 8, cursor + aligned)
    call memset(cursor, 0, size)
    return cursor
end

function function_reset():
    chunk = __lsl_qbe_load64(_function_arena_data)
    previous = 0
    retained = 0
    # Bound retained storage across arbitrarily many input functions. Large
    # dedicated mappings and excess normal chunks expire at freeall, while
    # at most 16 MiB of ordinary chunks remain available for the next function.
    while chunk != 0:
        next_chunk = __lsl_qbe_load64(chunk)
        mapped = __lsl_qbe_load64(chunk + 8)
        if mapped > 1048576 or retained >= 16777216:
            if previous == 0:
                __lsl_qbe_store64(_function_arena_data, next_chunk)
            else:
                __lsl_qbe_store64(previous, next_chunk)
            end
            if raw_unmap(chunk, mapped) != 0:
                Error(OSError: "QBE function arena deallocation failed")
            end
        else:
            retained += mapped
            previous = chunk
        end
        chunk = next_chunk
    end
    chunk = __lsl_qbe_load64(_function_arena_data)
    call __lsl_qbe_store64(_function_arena_data + 24, chunk)
    if chunk != 0:
        call __lsl_qbe_store64(_function_arena_data + 8, chunk + 16)
        call __lsl_qbe_store64(_function_arena_data + 16, chunk + __lsl_qbe_load64(chunk + 8))
    else:
        call __lsl_qbe_store64(_function_arena_data + 8, 0)
        call __lsl_qbe_store64(_function_arena_data + 16, 0)
    end
    return 0
end

function malloc(size):
    count = __lsl_qbe_raw_int(size)
    if count < 1:
        count = 1
    end
    if count > 1099511627776:
        Error(MemoryError: "QBE C allocation exceeds host limit")
    end
    if count <= 65520:
        index = 0
        slot = 32
        while slot < count + 16:
            slot *= 2
            index += 1
        end
        head_slot = _free_heads_data + index * 8
        base = __lsl_qbe_load64(head_slot)
        if base != 0:
            call __lsl_qbe_store64(head_slot, __lsl_qbe_load64(base + 16))
        else:
            next_slot = _slab_next_data + index * 8
            end_slot = _slab_end_data + index * 8
            base = __lsl_qbe_load64(next_slot)
            if base == __lsl_qbe_load64(end_slot):
                mapped = max(65536, slot * 16)
                base = raw_map(mapped)
                if base < 0:
                    Error(MemoryError: "QBE C slab allocation failed")
                end
                call __lsl_qbe_store64(end_slot, base + mapped)
            end
            call __lsl_qbe_store64(next_slot, base + slot)
        end
        call __lsl_qbe_store64(base, count)
        call __lsl_qbe_store64(base + 8, -index - 1)
        return base + 16
    end
    mapped = ((count + 16 + 4095) // 4096) * 4096
    base = raw_map(mapped)
    if base < 0:
        Error(MemoryError: "QBE C allocation failed")
    end
    call __lsl_qbe_store64(base, count)
    call __lsl_qbe_store64(base + 8, mapped)
    return base + 16
end

function free(raw_address):
    address = __lsl_qbe_raw_int(raw_address)
    if address != 0:
        base = address - 16
        mapped = __lsl_qbe_load64(base + 8)
        if mapped < 0:
            index = -mapped - 1
            head_slot = _free_heads_data + index * 8
            call __lsl_qbe_store64(address, __lsl_qbe_load64(head_slot))
            call __lsl_qbe_store64(head_slot, base)
            return
        end
        if raw_unmap(base, mapped) != 0:
            Error(OSError: "QBE C deallocation failed")
        end
    end
end

function load(raw_address, raw_size, signed=False):
    address = __lsl_qbe_raw_int(raw_address)
    size = __lsl_qbe_raw_int(raw_size)
    result = 0
    if size == 1:
        result = __lsl_qbe_load8(address)
    elif size == 2:
        result = __lsl_qbe_load16(address)
    elif size == 4:
        result = __lsl_qbe_load32(address)
    elif size == 8:
        return __lsl_qbe_load64(address)
    else:
        Error(ValueError: "invalid C scalar width")
    end
    if signed:
        sign = 1 << (size * 8 - 1)
        result = (result ~ sign) - sign
    end
    return result
end

function store(raw_address, raw_size, raw_value):
    address = __lsl_qbe_raw_int(raw_address)
    size = __lsl_qbe_raw_int(raw_size)
    value = __lsl_qbe_raw_int(raw_value)
    if size == 1:
        return __lsl_qbe_store8(address, value)
    elif size == 2:
        return __lsl_qbe_store16(address, value)
    elif size == 4:
        return __lsl_qbe_store32(address, value)
    elif size == 8:
        return __lsl_qbe_store64(address, value)
    end
    Error(ValueError: "invalid C scalar width")
end

function memcpy(raw_destination, raw_source, raw_count):
    destination = __lsl_qbe_raw_int(raw_destination)
    source = __lsl_qbe_raw_int(raw_source)
    count = __lsl_qbe_raw_int(raw_count)
    offset = 0
    while offset + 64 <= count:
        call __lsl_qbe_store64(destination + offset, __lsl_qbe_load64(source + offset))
        call __lsl_qbe_store64(destination + offset + 8, __lsl_qbe_load64(source + offset + 8))
        call __lsl_qbe_store64(destination + offset + 16, __lsl_qbe_load64(source + offset + 16))
        call __lsl_qbe_store64(destination + offset + 24, __lsl_qbe_load64(source + offset + 24))
        call __lsl_qbe_store64(destination + offset + 32, __lsl_qbe_load64(source + offset + 32))
        call __lsl_qbe_store64(destination + offset + 40, __lsl_qbe_load64(source + offset + 40))
        call __lsl_qbe_store64(destination + offset + 48, __lsl_qbe_load64(source + offset + 48))
        call __lsl_qbe_store64(destination + offset + 56, __lsl_qbe_load64(source + offset + 56))
        offset += 64
    end
    while offset + 8 <= count:
        call __lsl_qbe_store64(destination + offset, __lsl_qbe_load64(source + offset))
        offset += 8
    end
    while offset < count:
        call __lsl_qbe_store8(destination + offset, __lsl_qbe_load8(source + offset))
        offset += 1
    end
    return destination
end

function memmove(raw_destination, raw_source, raw_count):
    destination = __lsl_qbe_raw_int(raw_destination)
    source = __lsl_qbe_raw_int(raw_source)
    count = __lsl_qbe_raw_int(raw_count)
    if destination <= source or destination >= source + count:
        return memcpy(destination, source, count)
    end
    offset = count
    while offset >= 64:
        offset -= 64
        call __lsl_qbe_store64(destination + offset + 56, __lsl_qbe_load64(source + offset + 56))
        call __lsl_qbe_store64(destination + offset + 48, __lsl_qbe_load64(source + offset + 48))
        call __lsl_qbe_store64(destination + offset + 40, __lsl_qbe_load64(source + offset + 40))
        call __lsl_qbe_store64(destination + offset + 32, __lsl_qbe_load64(source + offset + 32))
        call __lsl_qbe_store64(destination + offset + 24, __lsl_qbe_load64(source + offset + 24))
        call __lsl_qbe_store64(destination + offset + 16, __lsl_qbe_load64(source + offset + 16))
        call __lsl_qbe_store64(destination + offset + 8, __lsl_qbe_load64(source + offset + 8))
        call __lsl_qbe_store64(destination + offset, __lsl_qbe_load64(source + offset))
    end
    while offset >= 8:
        offset -= 8
        call __lsl_qbe_store64(destination + offset, __lsl_qbe_load64(source + offset))
    end
    while offset > 0:
        offset -= 1
        call __lsl_qbe_store8(destination + offset, __lsl_qbe_load8(source + offset))
    end
    return destination
end

function memset(raw_destination, raw_byte, raw_count):
    destination = __lsl_qbe_raw_int(raw_destination)
    byte = __lsl_qbe_raw_int(raw_byte)
    count = __lsl_qbe_raw_int(raw_count)
    value = byte & 255
    word = value * 72340172838076673
    offset = 0
    while offset + 64 <= count:
        call __lsl_qbe_store64(destination + offset, word)
        call __lsl_qbe_store64(destination + offset + 8, word)
        call __lsl_qbe_store64(destination + offset + 16, word)
        call __lsl_qbe_store64(destination + offset + 24, word)
        call __lsl_qbe_store64(destination + offset + 32, word)
        call __lsl_qbe_store64(destination + offset + 40, word)
        call __lsl_qbe_store64(destination + offset + 48, word)
        call __lsl_qbe_store64(destination + offset + 56, word)
        offset += 64
    end
    while offset + 8 <= count:
        call __lsl_qbe_store64(destination + offset, word)
        offset += 8
    end
    while offset < count:
        call __lsl_qbe_store8(destination + offset, value)
        offset += 1
    end
    return destination
end

function memcmp(raw_left, raw_right, raw_count):
    left = __lsl_qbe_raw_int(raw_left)
    right = __lsl_qbe_raw_int(raw_right)
    count = __lsl_qbe_raw_int(raw_count)
    for offset in rang(count):
        delta = __lsl_qbe_load8(left + offset) - __lsl_qbe_load8(right + offset)
        if delta != 0:
            return delta
        end
    end
    return 0
end

function calloc(raw_count, raw_size):
    count = __lsl_qbe_raw_int(raw_count)
    size = __lsl_qbe_raw_int(raw_size)
    bytes = count * size
    result = malloc(bytes)
    call memset(result, 0, bytes)
    return result
end

function realloc(raw_address, raw_size):
    address = __lsl_qbe_raw_int(raw_address)
    size = __lsl_qbe_raw_int(raw_size)
    if address == 0:
        return malloc(size)
    end
    mapped = __lsl_qbe_load64(address - 8)
    capacity = mapped - 16
    if mapped < 0:
        capacity = (32 << (-mapped - 1)) - 16
    end
    if size > 0 and size <= capacity and size > capacity // 2:
        call __lsl_qbe_store64(address - 16, size)
        return address
    end
    result = malloc(size)
    call memcpy(result, address, min(size, __lsl_qbe_load64(address - 16)))
    call free(address)
    return result
end

function strlen(raw_address):
    address = __lsl_qbe_raw_int(raw_address)
    count = 0
    while __lsl_qbe_load8(address + count) != 0:
        count += 1
    end
    return count
end

function cstr(text):
    if text in _literals:
        return Int(_literals[text])
    end
    count = __lsl_text_byte_length(text)
    result = malloc(count + 1)
    call memcpy(result, __lsl_address(text) + 16, count)
    call __lsl_qbe_store8(result + count, 0)
    _literals[text] = result
    return result
end

function text(raw_address):
    address = __lsl_qbe_raw_int(raw_address)
    count = strlen(address)
    bytes = __lsl_byte_zeros(count)
    call memcpy(__lsl_data_address(bytes), address, count)
    return __lsl_text_from_bytes(bytes, 0, count)
end

function literal(text):
    # Only the translator's immutable C string literals use this registration.
    # General cstr/malloc pointers may be modified or freed and are not cached.
    # Tisíce literálů se registrují při každém startu: bez kopie. Řetězec LSL
    # leží na haldě jako [značka 8][délka 8][bajty][0], GC objekty nepřesouvá
    # a globální proměnná literál drží, takže C dostane přímo adresu bajtů
    # (včetně nuly na konci). Mapu pro literal_text sestaví až první dotaz.
    _literal_list.append(text)
    return __lsl_address(text) + 16
end

function literal_text(raw_address):
    global _literal_texts
    address = __lsl_qbe_raw_int(raw_address)
    if _literal_texts == None:
        _literal_texts = {}
        for registered in _literal_list:
            shown = registered
            terminator = String(registered).find(chr(0))
            if terminator >= 0:
                shown = String(registered)[0:terminator]
            end
            _literal_texts[__lsl_address(registered) + 16] = shown
        end
    end
    return _literal_texts.get(address, None)
end

function strcmp(raw_left, raw_right):
    left = __lsl_qbe_raw_int(raw_left)
    right = __lsl_qbe_raw_int(raw_right)
    offset = 0
    while True:
        a = __lsl_qbe_load8(left + offset)
        b = __lsl_qbe_load8(right + offset)
        if a != b or a == 0:
            return a - b
        end
        offset += 1
    end
end

function strncmp(raw_left, raw_right, raw_count):
    left = __lsl_qbe_raw_int(raw_left)
    right = __lsl_qbe_raw_int(raw_right)
    count = __lsl_qbe_raw_int(raw_count)
    for offset in rang(count):
        a = __lsl_qbe_load8(left + offset)
        b = __lsl_qbe_load8(right + offset)
        if a != b or a == 0:
            return a - b
        end
    end
    return 0
end

function strcpy(raw_destination, raw_source):
    destination = __lsl_qbe_raw_int(raw_destination)
    source = __lsl_qbe_raw_int(raw_source)
    return memcpy(destination, source, strlen(source) + 1)
end

function strncpy(raw_destination, raw_source, raw_count):
    destination = __lsl_qbe_raw_int(raw_destination)
    source = __lsl_qbe_raw_int(raw_source)
    count = __lsl_qbe_raw_int(raw_count)
    length = min(strlen(source), count)
    call memcpy(destination, source, length)
    call memset(destination + length, 0, count - length)
    return destination
end

function strchr(raw_address, raw_char):
    address = __lsl_qbe_raw_int(raw_address)
    char = __lsl_qbe_raw_int(raw_char)
    offset = 0
    while True:
        value = __lsl_qbe_load8(address + offset)
        if value == (char & 255):
            return address + offset
        elif value == 0:
            return 0
        end
        offset += 1
    end
end

function load_float(raw_address, single=False):
    address = __lsl_qbe_raw_int(raw_address)
    if single:
        return __lsl_qbe_loadf32(address)
    end
    return __lsl_float_from_bits(__lsl_qbe_load64(address))
end

function store_float(raw_address, value, single=False):
    address = __lsl_qbe_raw_int(raw_address)
    if single:
        return __lsl_qbe_storef32(address, value)
    end
    return __lsl_qbe_store64(address, __lsl_float_bits(value))
end
