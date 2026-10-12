#!/usr/bin/env python3
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CONF={'amd64_sysv':('cc',[]),'arm64':('aarch64-linux-gnu-gcc',['qemu-aarch64']),'rv64':('riscv64-linux-gnu-gcc',['qemu-riscv64'])}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();cc,runner=CONF[a.target];old=None;expected=None;diagnostics={}
 with tempfile.TemporaryDirectory(prefix='luxir-numeric-') as d:
  d=Path(d)
  for backend in a.backend:
   r=subprocess.run([str(backend.resolve()),'-t',a.target,str(ROOT/'tests/numeric.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
   assert old is None or old==r.stdout;old=r.stdout;asm=d/'numeric.s';asm.write_bytes(r.stdout);exe=d/'test'
   subprocess.run([cc,'-static','-no-pie','-O2','-frounding-math','-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/numeric_driver.c'),str(asm),'-lm','-o',str(exe)],check=True)
   out=subprocess.check_output(runner+[str(exe)],timeout=60);assert expected is None or expected==out;expected=out
  invalid=[('Int64','sqrt(Int64(1))'),('Float64','sqrt(Float64(1), Float64(2))'),('Float64','sqrt()'),('Int32','mulh(Int32(1), Int32(2))'),('Int64','mulh(Int64(1))'),('Float64','clz(Float64(1))'),('Int32','popcount(Int32(1), Int32(2))')]
  for backend in a.backend:
   for n,(ty,expr) in enumerate(invalid):
    source=d/'invalid.lir';source.write_text(f'export function f() -> {ty}:\n    {ty}(x) = {expr}\n    return x\nend\n')
    r=subprocess.run([str(backend.resolve()),'-t',a.target,str(source)],capture_output=True,timeout=10)
    assert r.returncode>0 and b'luxir:' in r.stderr and not r.stdout,(expr,r)
    assert n not in diagnostics or diagnostics[n]==r.stderr;diagnostics[n]=r.stderr
  print(a.target,expected.decode().strip(),f'; {len(invalid)} invalid forms rejected')
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'target':a.target,'output':expected.decode().strip(),'assembly_parity':True,'runner':runner,'invalid_forms':len(invalid)},indent=2)+'\n')
if __name__=='__main__':main()
