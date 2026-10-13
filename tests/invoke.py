#!/usr/bin/env python3
"""Explicit status-call error edges, indirect targets, SSA merges and syntax."""
import argparse,ctypes,json,os,random,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
ERRORS=[
 ('wide','function f():\n Int64(s) = invoke foo() else bad goto ok\n',b'invoke status result must be Int32'),
 ('float','function f():\n Float64(s) = invoke foo() else bad goto ok\n',b'invoke status result must be Int32'),
 ('missing-else','function f():\n invoke foo() goto ok\n',b'else expected after invoke call'),
 ('missing-success','function f():\n invoke foo() else bad\n',b'goto success label expected'),
 ('after-terminator','function f():\n invoke foo() else bad goto ok\n return\n',b'label or end expected after a jump'),
 ('undefined','function f():\n invoke foo() else absent goto ok\n',b'block absent is used undefined'),
 ('start','function f():\nstart:\n invoke foo() else start goto ok\n',b'invalid jump to the start block'),
]
RAW='''runtime_abi "1"
export data malformed = ["x", Int8(0)]
export function main() -> Int32:
    Ptr(output) = alloc8(8)
    Int32(status) = invoke luxrt_parse_i64(Ptr(malformed), Int64(1), output) else caught goto wrong
caught:
    Int32(ok) = eq(status, -22)
    if ok goto sizes else wrong
sizes:
    Int64(bytes) = call luxrt_heap_bytes()
    Ptr(heap) = alloc8(bytes)
    Int32(status) = invoke luxrt_heap_init(heap, Int64(1048576), Int64(4096)) else wrong goto tuple
 tuple:
    Int32(status) = invoke luxrt_tuple_new(heap, Ptr(0), Int64(0), output) else wrong goto destroy
 destroy:
    Int32(status) = invoke luxrt_heap_destroy(heap) else wrong goto done
 done:
    return 0
 wrong:
    return 1
end
'''
def verify(lib):
 u=ctypes.c_uint64;p=ctypes.POINTER(u);checks=0
 for name in ('invoke_status','invoke_discard'):
  f=getattr(lib,name);f.argtypes=[u,p,p];f.restype=ctypes.c_int32
 lib.invoke_pointer.argtypes=[ctypes.c_void_p,u,p,p];lib.invoke_pointer.restype=ctypes.c_int32
 lib.invoke_copy.argtypes=[u];lib.invoke_copy.restype=u
 lib.invoke_constant_error.restype=ctypes.c_int32;lib.invoke_constant_success.restype=ctypes.c_int32
 assert lib.invoke_constant_error()==-22 and lib.invoke_constant_success()==9;checks+=2
 rng=random.Random(0xE7707)
 for _ in range(20000):
  x=rng.getrandbits(64);selector=x&7;status=-selector if selector&1 else selector
  for name in ('invoke_status','invoke_discard','invoke_pointer'):
   f=getattr(lib,name);out=u(0x1029384756);count=u(0)
   args=[x,ctypes.byref(out),ctypes.byref(count)]
   if name=='invoke_pointer':args.insert(0,ctypes.cast(lib.invoke_probe,ctypes.c_void_p))
   got=f(*args);expect=9 if not status else (42 if name=='invoke_discard' else status)
   assert got==expect and count.value==1 and out.value==(0x1029384756 if status else x^0x98f91abe66543127);checks+=1
  assert lib.invoke_copy(x)==x;checks+=1
 return checks

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=['amd64_sysv','arm64','rv64'],default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();parity={};rows=[]
 with tempfile.TemporaryDirectory(prefix='luxir-invoke-') as d:
  d=Path(d)
  for b in a.backend:
   for flag in ([],['-i']):
    asm=d/'input.s';asm.write_bytes(subprocess.check_output([str(b.resolve())]+flag+[str(ROOT/'tests/invoke.lir')]))
    if flag:assert not re.search(rb'callq? always_(error|success)',asm.read_bytes())
    lib=d/('lib'+str(len(rows))+'.so');subprocess.run(['cc','-shared','-fPIC',str(asm),str(ROOT/'tests/invoke_probe.c'),'-o',str(lib)],check=True)
    rows.append({'backend':str(b),'inline':bool(flag),'checks':verify(ctypes.CDLL(str(lib)))})
   for t in TARGETS:
    asm=subprocess.check_output([str(b.resolve()),'-t',t,str(ROOT/'tests/invoke.lir')])
    if t in parity:assert parity[t]==asm
    parity[t]=asm
   for name,body,message in ERRORS:
    source=d/'bad.lir';source.write_text(body+'ok:\n return\nbad:\n return\nend\n')
    r=subprocess.run([str(b.resolve()),str(source)],capture_output=True)
    assert r.returncode and message in r.stderr,(name,r.stderr)
   source=d/'raw.lir';source.write_text(RAW);binary=d/'raw'
   subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(b.resolve()),'-t',a.target,'-o',str(binary),str(source)],check=True,timeout=90)
   runner={'amd64_sysv':[],'arm64':['qemu-aarch64'],'rv64':['qemu-riscv64']}[a.target]
   assert subprocess.run(runner+[str(binary)],timeout=20).returncode==0
 report={'target':a.target,'results':rows,'assembly_parity_targets':TARGETS,'syntax_rejections':len(ERRORS),'raw_managed_error_and_success_edges':True,'semantics':'Int32 status 0 succeeds, any nonzero follows explicit error edge; no unwinding'}
 if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
