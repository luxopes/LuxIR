#!/usr/bin/env python3
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CONF={'amd64_sysv':('cc',[]),'arm64':('aarch64-linux-gnu-gcc',['qemu-aarch64']),'rv64':('riscv64-linux-gnu-gcc',['qemu-riscv64'])}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();cc,runner=CONF[a.target]
 flags=[cc,'-static','-no-pie','-O2','-std=c11','-Wall','-Wextra','-Werror','-pthread','-DLUXIR_ATOMICS',str(ROOT/'tests/runtime/atomic_driver.c')]
 with tempfile.TemporaryDirectory(prefix='luxir-atomic-') as d:
  d=Path(d);exe=d/'test';subprocess.run(flags+[str(ROOT/'runtime/atomic.c'),str(ROOT/'tests/atomic_ir_oracle.c'),'-o',str(exe)],check=True);expected=subprocess.check_output(runner+[str(exe)],timeout=60);old=None;diagnostics={}
  for backend in a.backend:
   r=subprocess.run([str(backend.resolve()),'-t',a.target,str(ROOT/'tests/atomic_ir.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
   assert old is None or old==r.stdout;old=r.stdout;asm=d/'atomic.s';asm.write_bytes(r.stdout)
   assert b'luxrt_atomic' in r.stdout and b'call luxrt_atomic' not in r.stdout
   subprocess.run(flags+[str(asm),'-o',str(exe)],check=True);assert subprocess.check_output(runner+[str(exe)],timeout=60)==expected
  invalid=['Int64(x) = atomic_cas(p, Int64(1))','Float64(x) = atomic_cas(p, Float64(1), Float64(2))','Int64(x) = atomic_store(p, Int64(1))','Int32(x) = atomic_fence()','atomic_store(p, 1)','atomic_fence(Int64(1))','Float64(x) = atomic_load(p)','Int64(x) = atomic_add(p)']
  for backend in a.backend:
   for n,body in enumerate(invalid):
    source=d/'invalid.lir';source.write_text('export function invalid(Ptr(p)):\n    '+body+'\n    return\nend\n')
    r=subprocess.run([str(backend.resolve()),'-t',a.target,str(source)],capture_output=True,timeout=10);assert r.returncode>0 and r.stderr.startswith(b'luxir:') and not r.stdout,(body,r)
    assert n not in diagnostics or diagnostics[n]==r.stderr;diagnostics[n]=r.stderr
  print(a.target,expected.decode().strip(),f'; {len(invalid)} invalid forms rejected')
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'target':a.target,'output':expected.decode().strip(),'assembly_parity':True,'runtime_calls':False,'ordinary_memory_barrier_checks':2,'runner':runner,'invalid_forms':len(invalid)},indent=2)+'\n')
if __name__=='__main__':main()
