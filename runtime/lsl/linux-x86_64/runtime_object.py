"""Z výpisu runtime_dump.lsl (bajty, štítky, fixupy) udělá objekt runtime LSL.

Použití: runtime_object.py PREFIX  (čte PREFIX.bin/.labels/.fixups, píše
PREFIX.s a PREFIX.o). Viz runtime/lsl/README.md.
"""
import sys, pathlib, subprocess, re
prefix=pathlib.Path(sys.argv[1]); code=prefix.with_suffix('.bin').read_bytes()
labels={};fixups={}
for line in prefix.with_suffix('.labels').read_text().splitlines():
 pos,name=line.split(' ',1);labels.setdefault(int(pos),[]).append(name)
for line in prefix.with_suffix('.fixups').read_text().splitlines():
 pos,name=line.split(' ',1);fixups[int(pos)]=name
names={n for ns in labels.values() for n in ns}
missing=set(fixups.values())-names
if missing:raise SystemExit('unresolved runtime dependencies: '+repr(missing))
# Runtime začíná na hranici 64 B: zarovnání horkých rutin pak nezávisí
# na velikosti programu, který se linkuje před něj.
out=['.text','.p2align 6']
positions=sorted(set(labels)|set(fixups)|{len(code)})
pos=0
for boundary in positions:
 if boundary<pos:continue
 for i in range(pos,boundary,32):out.append('.byte '+','.join(str(b) for b in code[i:min(i+32,boundary)]))
 for name in labels.get(boundary,[]):out += ['.globl '+name,name+':']
 if boundary in fixups:out.append('.long '+fixups[boundary]+' - . - 4');pos=boundary+4
 else:pos=boundary
# SysV bridge: QBE may allocate RBX; old runtime uses it as its state base.
# Check the allocation budget in the bridge before saving RBX or making a
# second call. These scratch registers are caller-saved under SysV; all live
# QBE roots have already been placed in preserved registers/stack slots.
abi_source=(pathlib.Path(__file__).resolve().parent/'runtime_abi_linux_x86_64.lsl').read_text()
def abi_offset(name):
    match=re.search(r'^'+re.escape(name)+r' = (\d+)$',abi_source,re.M)
    if not match:raise SystemExit('missing runtime ABI offset: '+name)
    return int(match[1])
for name in sorted(names):
    if not name.startswith('runtime_'):
        continue
    # Můstky volané z každé iterace zarovnané jako běžné funkce (16 B).
    out += ['.p2align 4','.globl rt_'+name,'rt_'+name+':']
    if name == 'runtime_maybe_gc':
        out += ['mov lsl_state(%rip),%rax',
                f'mov {abi_offset("GC_ALLOCATED_BYTES")}(%rax),%r10',
                f'cmp {abi_offset("GC_BUDGET")}(%rax),%r10',
                'jge .Lqbe_maybe_gc_slow','ret','.Lqbe_maybe_gc_slow:']
    out += ['push %rbx','mov lsl_state(%rip),%rbx']
    if name == 'runtime_syscall6':
        # Seven SysV integer arguments: nr + six syscall operands. The saved
        # RBX and the bridge call must not replace the seventh stack argument.
        # Reserve 16 bytes to keep call alignment, and forward the operand.
        out += ['sub $16,%rsp','mov 32(%rsp),%rax','mov %rax,(%rsp)']
    out += ['call '+name]
    if name == 'runtime_syscall6':
        out += ['add $16,%rsp']
    out += ['pop %rbx','ret']
out += ['.globl _start','_start:','jmp qbe_runtime_start']
# qbe_runtime_start ends after initialization, then falls through here.
# Labels/wrappers above follow the runtime bytes; append a jump at byte end.
end_index=out.index('.globl rt_'+sorted(n for n in names if n.startswith('runtime_'))[0])
out[end_index:end_index]=['mov %rbx,lsl_state(%rip)','call lsl_entry','mov %rax,%rdi','jmp qbe_exit']
out += ['.globl qbe_error_flag','qbe_error_flag:','mov %fs:8,%rax','ret',
'.globl qbe_error_pending','qbe_error_pending:',
# Native runtime diagnostics live in static String records. Materialize one
# registered String on first inspection, keeping repeated last_error identity.
'mov %fs:0,%rax','test %rax,%rax','jz .Lqbe_pending_return',
'sub $8,%rsp','mov %rax,(%rsp)','mov %rax,%rdi',
'call rt_runtime_is_heap_object','test %rax,%rax','jnz .Lqbe_pending_heap',
'mov (%rsp),%rdi','call rt_runtime_string_clone','mov %rax,%fs:0',
'add $8,%rsp','ret','.Lqbe_pending_heap:','mov (%rsp),%rax',
'add $8,%rsp','.Lqbe_pending_return:','ret',
'.globl qbe_error_set','qbe_error_set:','mov %rdi,%fs:0','movq $1,%fs:8','xor %eax,%eax','ret',
'.globl qbe_error_catch','qbe_error_catch:','movq $0,%fs:8','xor %eax,%eax','ret',
'.globl qbe_error_clear','qbe_error_clear:','movq $0,%fs:0','movq $0,%fs:8','xor %eax,%eax','ret',
'.globl qbe_exit','qbe_exit:','push %rdi','call rt_runtime_stdout_flush','pop %rdi','mov $60,%rax','syscall',
'.bss','.align 8','.globl lsl_state','lsl_state:','.zero 8','.section .note.GNU-stack,"",@progbits']
prefix.with_suffix('.s').write_text('\n'.join(out)+'\n')
subprocess.run(['as','--64','-o',str(prefix.with_suffix('.o')),str(prefix.with_suffix('.s'))],check=True)
