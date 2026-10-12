#!/usr/bin/env python3
"""Execute luxrt C oracles and native LuxIR output under Linux qemu-user.

Requires cross GCC/libc + qemu. Static C drivers avoid dependence on host
cross sysroot loader paths. This does not execute a cross-built LSL frontend.
"""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
CONFIG={'arm64':('aarch64-linux-gnu-gcc','qemu-aarch64'),'rv64':('riscv64-linux-gnu-gcc','qemu-riscv64')}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--target',choices=CONFIG,required=True);ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--cc');ap.add_argument('--runner');ap.add_argument('--report',type=Path);a=ap.parse_args()
 cc,runner=CONFIG[a.target];cc=a.cc or cc;runner=a.runner or runner;platform=ROOT/('runtime/linux-'+a.target);cases=[]
 flags=[cc,'-static','-no-pie','-O2','-std=c11','-Wall','-Wextra','-Werror']
 with tempfile.TemporaryDirectory(prefix='luxrt-cross-') as d:
  d=Path(d);exe=d/'test'
  for name,modules,extra in [('memory',['memory'],[]),('integer',['integer'],[]),('checked',['checked'],[]),('startup',['startup'],[]),('os',['os'],['syscall.S']),('io',['io','os'],['syscall.S'])]:
   driver=ROOT/('tests/runtime/'+name+'_driver.c');assembly_extra=[platform/file for file in extra]
   ref=[(platform if module=='os' else ROOT/'runtime')/(module+'.c') for module in modules]
   subprocess.run(flags+list(map(str,[driver,*ref,*assembly_extra]))+['-o',str(exe)],check=True,capture_output=True)
   expected=subprocess.check_output([runner,str(exe)],timeout=90);print(a.target,expected.decode().strip(),flush=True);old={}
   for backend in a.backend:
    asm=[]
    for module in modules:
     source=(platform if module=='os' else ROOT/'runtime')/(module+'.lir')
     r=subprocess.run([str(backend.resolve()),'-t',a.target,str(source)],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
     assert module not in old or old[module]==r.stdout;old[module]=r.stdout;p=d/(module+'.s');p.write_bytes(r.stdout);asm.append(p)
    subprocess.run(flags+list(map(str,[driver,*asm,*assembly_extra]))+['-o',str(exe)],check=True,capture_output=True)
    assert subprocess.check_output([runner,str(exe)],timeout=90)==expected
   cases.append({'module':name,'output':expected.decode().strip(),'assembly_parity':True,'execution':True})
  outputs=[]
  for source in [ROOT/'runtime/atomic.c',platform/'atomic.S']:
   subprocess.run(flags+['-pthread',str(ROOT/'tests/runtime/atomic_driver.c'),str(source),'-o',str(exe)],check=True,capture_output=True)
   outputs.append(subprocess.check_output([runner,str(exe)],timeout=90))
  assert outputs[0]==outputs[1];print(a.target,outputs[0].decode().strip(),flush=True)
  cases.append({'module':'atomic','output':outputs[0].decode().strip(),'execution':True})
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'target':a.target,'runner':runner,'cc':cc,'cases':cases,'cross_selfhost':False},indent=2)+'\n')
 print('Linux cross runtime tests passed:',a.target)
if __name__=='__main__':main()
