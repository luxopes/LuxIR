# lsl: internal-abi
# Relokace x86_64 patří instrukčnímu backendu, nikoli formátu obrazu.

load bytes

function patch_relative32(code, patch, reference_end, target, addend=0):
    if patch < 0 or patch > len(code) - 4:
        Error(AssemblerError: "relative relocation patch is outside code")
    end
    displacement = target + addend - reference_end
    if displacement < -2147483648 or displacement > 2147483647:
        Error(AssemblerError: "relative relocation exceeds signed 32-bit range")
    end
    call bytes.patch_little(code, patch, displacement, 4)
end

function data_reference(patch, reference_end, target_offset):
    return {"kind": "pc_rel32", "width": 4, "section": "code", "offset": patch, "base": reference_end, "target_section": "data", "target_offset": target_offset, "addend": 0}
end

function apply(program, code, section_addresses):
    if program["architecture"] != "x86_64":
        Error(AssemblerError: "x86_64 relocation handler requires x86_64 code")
    end
    for relocation in program["relocations"]:
        if relocation["kind"] != "pc_rel32" or relocation.get("width", 4) != 4 or relocation["section"] != "code":
            Error(AssemblerError: "unsupported x86_64 relocation")
        end
        target_section = relocation["target_section"]
        if target_section not in section_addresses:
            Error(AssemblerError: "relocation target section is missing")
        end
        target = section_addresses[target_section] + relocation["target_offset"]
        reference_end = section_addresses["code"] + relocation["base"]
        call patch_relative32(code, relocation["offset"], reference_end, target, relocation.get("addend", 0))
    end
end
