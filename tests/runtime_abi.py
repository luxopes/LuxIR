#!/usr/bin/env python3
"""ABI 1 headers retain linker guards, auto-link managed modules, enforce revision."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CONFIG={'amd64_sysv':('',[]),'arm64':('aarch64-linux-gnu-',['qemu-aarch64']),'rv64':('riscv64-linux-gnu-',['qemu-riscv64'])}
SOURCE='''ir_version "1"
runtime_abi "1"
export function main() -> Int32:
    Int64(revision) = call luxrt_abi_revision()
    Int32(ok) = eq(revision, Int64(1))
    if ok goto sizes else fail
sizes:
    Int64(heap) = call luxrt_heap_bytes()
    Int64(roots) = call luxrt_roots_bytes()
    Int64(desc) = call luxrt_descriptor_bytes()
    Int32(a) = eq(heap, Int64(336))
    Int32(b) = eq(roots, Int64(24))
    Int32(c) = eq(desc, Int64(48))
    Int32(ok) = and(a, b)
    Int32(ok) = and(ok, c)
    if ok goto done else fail
done:
    return 0
fail:
    return 1
end
'''
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONFIG,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();prefix,runner=CONFIG[a.target];checks=0;reference=None
 with tempfile.TemporaryDirectory(prefix='luxir-abi-') as d:
  d=Path(d);source=d/'test.lir';source.write_text(SOURCE);obj=d/'input.o';asm=d/'input.s';previous=None
  for b in a.backend:
   command=[str(b.resolve()),'-t',a.target]+(['-p'] if a.target=='rv64' else [])
   blob=subprocess.check_output(command+[str(source)])
   assert b'luxrt_abi_v1_le64_explicit' in blob;checks+=1
   if reference is not None:assert blob==reference
   reference=blob;asm.write_bytes(blob)
   subprocess.run([prefix+'as',str(asm),'-o',str(obj)],check=True)
   # Missing guard must fail even if all ordinary sections are dead.
   result=subprocess.run([prefix+'ld','--gc-sections','-e','main',str(obj),'-o',str(d/'missing')],capture_output=True)
   assert result.returncode and b'luxrt_abi_v1_le64_explicit' in result.stderr;checks+=1
   empty=d/'empty.lir';empty.write_text('runtime_abi "1"\n')
   asm.write_bytes(subprocess.check_output(command+[str(empty)]))
   subprocess.run([prefix+'as',str(asm),'-o',str(d/'empty.o')],check=True)
   wrong=d/'wrong.s';wrong.write_text('.data\n.globl luxrt_abi_v2_le64_explicit\nluxrt_abi_v2_le64_explicit: .quad 2\n')
   subprocess.run([prefix+'as',str(wrong),'-o',str(d/'wrong.o')],check=True)
   result=subprocess.run([prefix+'ld','--gc-sections',str(d/'empty.o'),str(d/'wrong.o'),'-o',str(d/'wrong')],capture_output=True)
   assert result.returncode and b'luxrt_abi_v1_le64_explicit' in result.stderr;checks+=1
   # Per-file reset: ABI 1 then none has exactly one guard.
   none=d/'none.lir';none.write_text('runtime_abi "none"\n')
   pair=subprocess.check_output(command+[str(empty),str(none)])
   assert pair.count(b'.quad luxrt_abi_v1_le64_explicit')==1;checks+=1
   for target in ['amd64_win','amd64_apple','arm64_apple']:
    result=subprocess.run([str(b.resolve()),'-t',target,str(empty)],capture_output=True)
    assert result.returncode and b'ABI 1 requires an ELF target' in result.stderr and not result.stdout;checks+=1
   binary=d/'raw'
   subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--inline','--backend',str(b.resolve()),'-t',a.target,'-o',str(binary),str(source)],check=True,timeout=90)
   assert subprocess.run(runner+[str(binary)],timeout=20).returncode==0;checks+=1
   content=binary.read_bytes();assert previous is None or content==previous;previous=content
   # The header alone adds the managed module set: do not pass --managed.
   managed=d/'managed.lir';managed.write_text((ROOT/'tests/runtime/extended_entry.lir').read_text().replace('runtime_abi "none"','runtime_abi "1"'))
   subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(b.resolve()),'-t',a.target,'-o',str(binary),str(managed),str(ROOT/'tests/runtime/extended_cases.lir')],check=True,timeout=90)
   assert subprocess.run(runner+[str(binary)],timeout=60).returncode==0;checks+=1
   # Every advertised function must actually link on its declared platform.
   manifest=json.loads((ROOT/'runtime/abi-v1.json').read_text())
   symbols=[f['name'] for f in manifest['functions'] if a.target in f['targets']]
   table=d/'symbols.lir'
   program='runtime_abi "1"\nexport data all_abi_symbols = ['+', '.join('Ptr('+name+')' for name in symbols)+']\n'
   program+='export function main() -> Int32:\nstart:\n goto loop\nloop:\n Int64(i) = phi(start: 0, next: inc)\n Int64(offset) = mul(i, 8)\n Ptr(slot) = add(all_abi_symbols, offset)\n Ptr(value) = load(slot)\n Int32(empty) = eq(value, 0)\n if empty goto fail else next\nnext:\n Int64(inc) = add(i, 1)\n Int32(more) = lt(inc, Int64('+str(len(symbols))+'))\n if more goto loop else done\ndone:\n return 0\nfail:\n return 1\nend\n'
   table.write_text(program)
   subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(b.resolve()),'-t',a.target,'-o',str(binary),str(table)],check=True,timeout=90)
   assert subprocess.run(runner+[str(binary)],timeout=60).returncode==0;checks+=1
   # Rejected ABI must preserve an existing binary in the build driver.
   old=binary.read_bytes();source.write_text(SOURCE.replace('runtime_abi "1"','runtime_abi "2"'))
   result=subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(b.resolve()),'-o',str(binary),str(source)],capture_output=True)
   assert result.returncode and old==binary.read_bytes();checks+=1
   source.write_text(SOURCE)
 report={'target':a.target,'checks':checks,'backends':list(map(str,a.backend)),'retained_guard':'luxrt_abi_v1_le64_explicit','gc_sections_missing_runtime_rejected':True,'auto_managed_modules':True,'raw_binary_identical':True,'advertised_symbols_linked':len(symbols),'unsupported_platforms_rejected':True,'runner':runner}
 if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
