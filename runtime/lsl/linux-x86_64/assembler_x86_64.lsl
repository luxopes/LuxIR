# lsl: internal-abi
# Tiny label/fixup assembler data structure implemented in LSL. It emits bytes;
# it never invokes an assembler executable.
#
# The code buffer is a ByteArray: it stores raw bytes instead of one boxed Int
# per byte, so the collector never has to traverse the emitted program and
# appending is a plain store. Instruction encoders still hand over short
# List[Int] values; emit copies them in a typed loop.

load bytes
load relocations_x86_64

function builder():
    return {"code": ByteArray(), "labels": {}, "fixups": [], "image_relocations": [], "imports": [], "symbols": {}}
end

function position(value):
    return len(ByteArray(value["code"]))
end

function emit(value, data):
    if len(data) == 4 and data[1] == 131 and data[3] == 0 and (data[0] == 72 or data[0] == 73) and (data[2] & 248) == 248:
        # Peephole: cmp r64, 0 -> test r64, r64. Identical flags for every
        # jcc/setcc and one byte shorter. (Store-to-load elimination and
        # jump-to-next removal were measured 8-10% slower on perft and the
        # self-host build on this target, so they are deliberately absent.)
        register = data[2] & 7
        rex = 72
        if data[0] == 73:
            rex = 77
        end
        data = [rex, 133, 192 + register * 8 + register]
    end
    call bytes.extend(value["code"], data)
end

function label(value, name):
    if name in value["labels"]:
        Error(AssemblerError: "duplicate label")
    end
    labels = value["labels"]
    labels[name] = position(value)
end

function relative(value, opcode, name):
    code = ByteArray(value["code"])
    call bytes.extend(code, opcode)
    patch = len(code)
    call code.append(0)
    call code.append(0)
    call code.append(0)
    call code.append(0)
    value["fixups"].append([patch, name])
end

function finish(value):
    code = ByteArray(value["code"])
    for fixup in value["fixups"]:
        if fixup[1] not in value["labels"]:
            Error(AssemblerError: "unknown label")
        end
        target = value["labels"][fixup[1]]
        call relocations_x86_64.patch_relative32(code, fixup[0], fixup[0] + 4, target)
    end
    return code
end

# OS runtime může deklarovat import; image writer určí jeho skutečnou adresu.
function import_symbol(value, library, name):
    symbol = String(library) + "!" + String(name)
    for item in value["imports"]:
        if item["symbol"] == symbol:
            return symbol
        end
    end
    value["imports"].append({"symbol": symbol, "library": String(library), "name": String(name)})
    return symbol
end

function external_relative(value, opcode, symbol):
    call emit(value, opcode)
    patch = position(value)
    call emit(value, [0, 0, 0, 0])
    value["image_relocations"].append({"kind": "import_pc_rel32", "width": 4, "section": "code", "offset": patch, "base": patch + 4, "symbol": String(symbol), "addend": 0})
end

function export_symbol(value, name):
    if name in value["symbols"]:
        Error(AssemblerError: "duplicate exported symbol")
    end
    value["symbols"][name] = {"section": "code", "offset": position(value)}
end
