# lsl: internal-abi
# Vypíše runtime LSL (ABI lsl-0) jako bajty, štítky a fixupy pro runtime_object.py.
load runtime_linux_x86_64 as runtime
load assembler_x86_64 as assembler
load x86_64
output = assembler.builder()
call runtime.emit_all(output)
call runtime.emit_lazy(output,True,True)
call assembler.label(output,"qbe_runtime_start")
call runtime.emit_entry(output,0,524288,65535)
call writebytes(arg(1)+".bin",output["code"])
lines = []
for name in output["labels"]:
    lines.append(String(output["labels"][name])+" "+name+chr(10))
end
call writefile(arg(1)+".labels","".join(lines))
lines = []
for fixup in output["fixups"]:
    lines.append(String(fixup[0])+" "+fixup[1]+chr(10))
end
call writefile(arg(1)+".fixups","".join(lines))
