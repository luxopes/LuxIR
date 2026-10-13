# lsl: internal-abi
# Soukromé volací ABI LSL pro x86_64, nezávislé na operačním systému. Není veřejným plugin ABI.
# Čísla registrů jsou identifikátory encoderu x86_64, nikoli součást frontendu.

NAME = "lsl-x86_64-v1"
WORD_BYTES = 8
STACK_ALIGNMENT = 8
FRAME_ALIGNMENT = 16
FUNCTION_ALIGNMENT = 32
ARGUMENT_REGISTERS = [7, 6, 2, 1, 8, 9]
LOCAL_REGISTERS = [12, 13, 14, 15]
SCRATCH_REGISTERS = [1, 2, 6, 7, 8, 9]
RESULT_REGISTER = 0
STATE_REGISTER = 3
STACK_REGISTER = 4
FRAME_REGISTER = 5
STACK_ARGUMENT_BASE = 16
OBJECT_TAG = 0
OBJECT_LENGTH = 8
OBJECT_CAPACITY = 16
OBJECT_DATA = 24
COLLECTION_HEADER_BYTES = 32

function frame_bytes(slots):
    size = slots * WORD_BYTES
    while size % FRAME_ALIGNMENT != 0:
        size += 1
    end
    return size
end

function local_offset(binding):
    return -binding * WORD_BYTES
end

function incoming_argument_offset(index):
    return STACK_ARGUMENT_BASE + (index - len(ARGUMENT_REGISTERS)) * WORD_BYTES
end

function global_offset(binding, state_bytes):
    return state_bytes + (binding - 1) * WORD_BYTES
end

function describe():
    return {"name": NAME, "word_bytes": WORD_BYTES, "stack_alignment": STACK_ALIGNMENT, "frame_alignment": FRAME_ALIGNMENT, "arguments": ARGUMENT_REGISTERS, "result": RESULT_REGISTER, "state": STATE_REGISTER, "locals": LOCAL_REGISTERS}
end

# Kontrola CPU ABI nevybírá OS, TLS ani image format.
function matches(target):
    return target.get("abi", "") == NAME and target.get("architecture", "") == "x86_64" and target.get("pointer_bytes", 0) == WORD_BYTES and target.get("byte_order", "") == "little"
end
