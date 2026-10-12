#!/usr/bin/env python3
"""Regression for local symbol + offset and external addresses in rv64 static PIE."""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MAIN='''ir_version "1"
data local = [Int64(101, 202)]
export function luxrt_entry_main(Ptr(stack)) -> Int32:
    Ptr(p) = add(Ptr(local), 8)
    Ptr(p) = call pointer_identity(p)
    Int64(a) = load(p)
    Ptr(q) = copy(Ptr(external))
    Ptr(q) = call pointer_identity(q)
    Int64(b) = load(q)
    Int64(total) = add(a, b)
    Int32(ok) = eq(total, 505)
    Int32(status) = xor(ok, 1)
    return status
end
'''
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);a=ap.parse_args();old=[]
 with tempfile.TemporaryDirectory(prefix='luxir-rv64-pie-') as d:
  d=Path(d);source=d/'main.lir';source.write_text(MAIN);external=d/'external.lir';external.write_text('export data external = Int64(303)\nexport function pointer_identity(Ptr(p)) -> Ptr:\n    return p\nend\n');baseline=None
  for index,backend in enumerate(a.backend):
   obj=[];assembly=[]
   for name,path in [('main',source),('external',external)]:
    r=subprocess.run([str(backend.resolve()),'-t','rv64','-p',str(path)],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    if name=='main':assert b'lla ' in r.stdout and b'%hi(' not in r.stdout
    assembly.append(r.stdout);s=d/(name+'.s');s.write_bytes(r.stdout);o=d/(name+'.o');subprocess.run(['riscv64-linux-gnu-as','-o',str(o),str(s)],check=True);obj.append(o)
   if old:assert old==assembly
   old=assembly;entry=d/'entry.o';subprocess.run(['riscv64-linux-gnu-as','-o',str(entry),str(ROOT/'runtime/linux-rv64/entry.S')],check=True);exe=d/'image'
   subprocess.run(['riscv64-linux-gnu-ld','-static','-pie','--no-dynamic-linker','-z','text','-z','noexecstack','--build-id=none','-s','-o',str(exe),*map(str,obj),str(entry)],check=True)
   subprocess.run(['qemu-riscv64',str(exe)],check=True,timeout=10);image=exe.read_bytes();assert baseline is None or baseline==image;baseline=image
   reloc=subprocess.check_output(['riscv64-linux-gnu-readelf','-r',str(exe)],text=True);assert 'There are no relocations' in reloc,reloc
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'target':'rv64','static_pie':True,'local_symbol_offset':True,'external_address':True,'assembly_parity':True,'binary_parity':True,'runtime_relocations':False,'qemu_execution':True},indent=2)+'\n')
 print('rv64 local/external PC-relative static PIE regression passed')
if __name__=='__main__':main()
