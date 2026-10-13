#!/usr/bin/env python3
"""Build freestanding Linux IR with bundled raw luxrt and external GNU as/ld.

This is a developer driver, not the planned built-in object writer/linker or
managed ABI. Inputs contain main(argc,argv,envp)->Int32; unused args may be
omitted. Main TLS and kernel entry are supplied by this driver.
"""
import argparse,os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
TARGETS={'amd64_sysv':('x86_64',''),'arm64':('arm64','aarch64-linux-gnu-'),'rv64':('rv64','riscv64-linux-gnu-')}
ADAPTER=r'''ir_version "1"
runtime_abi "none"
export data luxrt_main_stack_top = Ptr(0)
data startup_error = ["LuxIR: TLS initialization failed\n", Int8(0)]
export function luxrt_entry_main(Ptr(stack)) -> Int32:
    store(stack, luxrt_main_stack_top)
    Ptr(block) = alloc8(24)
    Int64(rc) = call luxrt_tls_main(stack, block)
    Int32(ok) = eq(rc, 0)
    if ok goto main else failed
main:
    Int64(argc) = call luxrt_startup_argc(stack)
    Ptr(argv) = call luxrt_startup_argv(stack)
    Ptr(envp) = call luxrt_startup_env(stack)
    Int32(status) = call ENTRY(Int32(argc), argv, envp)
    return status
failed:
    Int64(written) = call luxrt_os_write(Int64(2), Ptr(startup_error), Int64(33))
    return 127
end
'''
def sections(assembly,module,target,gates=False):
    """Split assembler gates; backend -s handles IR linkage precisely."""
    kind='%progbits' if target=='arm64' else '@progbits';counter=0
    def replace(m):
        nonlocal counter
        counter+=1
        section=f'.section .text.luxir_{module}_{counter},"ax",{kind}'
        return section+'\n'+m.group(0) if gates else section
    pattern=r'(?m)^\.globl\s+\w+\s*$' if gates else r'(?m)^\.text$'
    return re.sub(pattern,replace,assembly)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('inputs',type=Path,nargs='+');ap.add_argument('-o','--output',type=Path,required=True)
    ap.add_argument('-t','--target',choices=TARGETS,default='amd64_sysv')
    ap.add_argument('--backend',type=Path,default=ROOT/'build/luxir')
    ap.add_argument('--entry',default='main');ap.add_argument('--keep-runtime',action='store_true',help='disable section garbage collection')
    ap.add_argument('--managed',action='store_true',help='link experimental single-owner managed core and collections (explicit roots)')
    ap.add_argument('--inline',action='store_true',help='inline small locally bound definitions within each IR input')
    a=ap.parse_args()
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*',a.entry):ap.error('entry must be a plain IR symbol')
    if not a.backend.is_file():ap.error('backend missing; build LuxIR or pass --backend')
    for p in a.inputs:
        if not p.is_file():ap.error('input missing: '+str(p))
    arch,prefix=TARGETS[a.target];platform=ROOT/('runtime/linux-'+arch)
    sources=[ROOT/('runtime/'+name+'.lir') for name in ['memory','integer','checked','startup','io','tls','relocate','arena','float64','abi','process','capture','hash']]
    sources+=[platform/(name+'.lir') for name in ['os','thread','signal','tls-native']]
    if a.managed or any(re.search(r'^\s*runtime_abi\s+"1"(?:\s|$)', p.read_text(), re.M) for p in a.inputs):
        sources += [ROOT/('runtime/'+name+'.lir') for name in ['managed','collections','map','numeric','tuple','secure']]
    destination=a.output.resolve();destination.parent.mkdir(parents=True,exist_ok=True)
    # Final output is replaced only after successful compile and link.
    with tempfile.TemporaryDirectory(prefix='.luxir-build-',dir=destination.parent) as tmp:
        tmp=Path(tmp);adapter=tmp/'entry.lir';adapter.write_text(ADAPTER.replace('ENTRY',a.entry))
        objects=[]
        for n,source in enumerate(a.inputs+[adapter]+sources):
            cmd=[str(a.backend.resolve()),'-t',a.target,'-s']+(['-i'] if a.inline else [])+(['-p'] if a.target=='rv64' else [])+[str(source.resolve())]
            r=subprocess.run(cmd,capture_output=True,timeout=60)
            if r.returncode:raise SystemExit(r.stderr.decode(errors='replace'))
            asm=tmp/(str(n)+'.s');asm.write_text(sections(r.stdout.decode(),str(n),a.target));obj=asm.with_suffix('.o')
            subprocess.run([prefix+'as','-o',str(obj),str(asm)],check=True,timeout=30);objects.append(obj)
        for n,name in enumerate(['entry-relocate','syscall','atomic','thread','tls','context','signal']):
            asm=tmp/('gate'+str(n)+'.s');asm.write_text(sections((platform/(name+'.S')).read_text(),'gate'+str(n),a.target,True));obj=asm.with_suffix('.o')
            subprocess.run([prefix+'as','-o',str(obj),str(asm)],check=True,timeout=30);objects.append(obj)
        binary=tmp/'program'
        cmd=[prefix+'ld','-static','-pie','--no-dynamic-linker','-z','text','-z','relro','-z','noexecstack','--build-id=none','-s','-e','_start']
        if not a.keep_runtime:cmd+=['--gc-sections']
        subprocess.run(cmd+['-o',str(binary),*map(str,objects)],check=True,timeout=30)
        binary.chmod(0o755);os.replace(binary,destination)
if __name__=='__main__':main()
