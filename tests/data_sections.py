#!/usr/bin/env python3
"""Linker reachability tests for data, BSS, TLS, literals and custom sections."""
import argparse,hashlib,json,struct,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CONF={'amd64_sysv':('',[]),'arm64':('aarch64-linux-gnu-',['qemu-aarch64']),'rv64':('riscv64-linux-gnu-',['qemu-riscv64'])}
DEAD=b'LUXIR_UNREACHABLE_DATA_MARKER!'
CUSTOM=b'LUXIR_EXPLICIT_GROUP_MARKER!'
LIVE_FLOAT=1.23456789012345
DEAD_FLOAT=9.87654321098765
SOURCE='''ir_version "1"
runtime_abi "none"
data live_bytes align 32 = [Int8(0, 42), zero(30)]
data live_pointer = Ptr(live_bytes + 1)
data live_chain = Ptr(live_pointer)
data live_callback = Ptr(callback)
data live_bss = zero(8)
common data live_common = zero(8)
thread data live_tls align 64 = Int64(41)
thread data live_tls_pointer = Ptr(live_bytes + 1)
thread data live_tbss align 32 = zero(8)
section ".luxir_group" "aw" data custom_live = Int64(42)
section ".luxir_group" "aw" data custom_group = "LUXIR_EXPLICIT_GROUP_MARKER!"
data dead_bytes = ["LUXIR_UNREACHABLE_DATA_MARKER!", zero(65536)]
data dead_bss = zero(1048576)
thread data dead_tls align 64 = ["LUXIR_UNREACHABLE_DATA_MARKER!", zero(32768)]
thread data dead_tbss = zero(65536)
data dead_cycle_a = Ptr(dead_cycle_b)
data dead_cycle_b = Ptr(dead_cycle_a)
function callback() -> Float64:
    return 1.23456789012345
end
function dead_function() -> Float64:
    Ptr(p) = load(dead_cycle_a)
    Int32(x) = load_u8(p)
    Float64(f) = itof(x)
    Float64(f) = add(f, 9.87654321098765)
    return f
end
export function main() -> Int32:
    Ptr(p) = load(live_chain)
    Ptr(p) = load(p)
    Int32(value) = load_u8(p)
    Int32(ok) = eq(value, 42)
    Ptr(tp) = load(thread(live_tls_pointer))
    Int32(same) = eq(tp, p)
    Int32(ok) = and(ok, same)
    Int64(t) = load(thread(live_tls))
    Int32(same) = eq(t, 41)
    Int32(ok) = and(ok, same)
    Int64(t) = load(thread(live_tbss))
    Int32(same) = eq(t, 0)
    Int32(ok) = and(ok, same)
    store(Int64(42), thread(live_tbss))
    Int64(t) = load(thread(live_tbss))
    Int32(same) = eq(t, 42)
    Int32(ok) = and(ok, same)
    Int64(z) = load(live_bss)
    Int32(same) = eq(z, 0)
    Int32(ok) = and(ok, same)
    store(Int64(42), live_bss)
    Int64(z) = load(live_common)
    Int32(same) = eq(z, 0)
    Int32(ok) = and(ok, same)
    store(Int64(42), live_common)
    Int64(c) = load(custom_live)
    Int32(same) = eq(c, 42)
    Int32(ok) = and(ok, same)
    Ptr(fn) = load(live_callback)
    Float64(f) = call fn()
    Int32(same) = eq(f, 1.23456789012345)
    Int32(ok) = and(ok, same)
    Int32(status) = xor(ok, 1)
    return status
end
'''
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();prefix,runner=CONF[a.target];rows=[];checks=0
 with tempfile.TemporaryDirectory(prefix='luxir-data-sections-') as d:
  d=Path(d);source=d/'test.lir';source.write_text(SOURCE);previous=None
  for index,backend in enumerate(a.backend):
   # Inspect the unlinked object as well: a dead symbol may not be confused
   # with an optimizer removing the data before the linker sees it.
   cmd=[str(backend.resolve()),'-t',a.target,'-s']+(['-p'] if a.target=='rv64' else [])+[str(source)]
   asm=subprocess.check_output(cmd,text=True);asmfile=d/'test.s';asmfile.write_text(asm);obj=d/'test.o';subprocess.run([prefix+'as','-o',str(obj),str(asmfile)],check=True)
   info=subprocess.check_output([prefix+'readelf','-W','-S',str(obj)],text=True)
   for name in ['.data.luxir_','.bss.luxir_','.tdata.luxir_','.tbss.luxir_','.rodata.luxir_fp','.luxir_group']:
    assert name in info,(a.target,name,info);checks+=1
   assert '.section ".luxir_group","aw"\n' in asm;checks+=1
   symbols=subprocess.check_output([prefix+'nm',str(obj)],text=True);assert 'dead_bytes' in symbols and 'dead_function' in symbols;checks+=1
   outputs=[]
   for keep in [False,True]:
    exe=d/('program'+str(index)+str(keep));cmd=['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(backend.resolve()),'-t',a.target,'-o',str(exe),str(source)]+(['--keep-runtime'] if keep else [])
    subprocess.run(cmd,check=True,timeout=60);r=subprocess.run(runner+[str(exe)],capture_output=True,timeout=10);assert r.returncode==0 and not r.stdout and not r.stderr,(a.target,r);checks+=1
    outputs.append(exe.read_bytes())
   small,full=outputs
   assert DEAD not in small and DEAD in full;checks+=1
   assert CUSTOM in small and CUSTOM in full;checks+=1
   assert struct.pack('<d',DEAD_FLOAT) not in small and struct.pack('<d',DEAD_FLOAT) in full;checks+=1
   assert struct.pack('<d',LIVE_FLOAT) in small;checks+=1
   assert len(full)-len(small)>65536;checks+=1
   assert previous is None or previous==small;previous=small
   rows.append({'backend':str(backend),'bytes':len(small),'bytes_without_dce':len(full),'sha256':hashlib.sha256(small).hexdigest()})
   # Unsupported output formats must fail explicitly; default output remains
   # unchanged and custom sections remain grouped by frontend intent.
   for target in ['amd64_win','amd64_apple','arm64_apple']:
    r=subprocess.run([str(backend.resolve()),'-s','-t',target,str(source)],capture_output=True);assert r.returncode!=0 and b'-s requires an ELF target' in r.stderr;checks+=1
  report={'target':a.target,'checks':checks,'backends':rows,'binary_identical':True,'data_tls_bss_literal_dce':True,'transitive_pointer_and_callback_roots':True,'explicit_sections_preserved':True,'runner':runner}
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
  print(json.dumps(report))
if __name__=='__main__':main()
