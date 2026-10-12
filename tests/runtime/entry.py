#!/usr/bin/env python3
"""Actual kernel argv/envp/auxv in a libc-free static PIE, not a synthetic stack."""
import argparse,json,subprocess,tempfile,os
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=['amd64_sysv','arm64','rv64'],default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args()
 arch={'amd64_sysv':'x86_64','arm64':'arm64','rv64':'rv64'}[a.target];prefix={'amd64_sysv':'','arm64':'aarch64-linux-gnu-','rv64':'riscv64-linux-gnu-'}[a.target];run={'amd64_sysv':[],'arm64':['qemu-aarch64'],'rv64':['qemu-riscv64']}[a.target]
 with tempfile.TemporaryDirectory(prefix='luxrt-entry-') as d:
  d=Path(d);old=None
  for index,backend in enumerate(a.backend):
   objects=[]
   for module,source in [('demo','examples/startup-demo.lir'),('startup','runtime/startup.lir'),('integer','runtime/integer.lir'),('os','runtime/linux-'+arch+'/os.lir')]:
    r=subprocess.run([str(backend.resolve()),'-t',a.target,*(['-p'] if a.target=='rv64' else []),str(ROOT/source)],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    asm=d/(module+'.s');asm.write_bytes(r.stdout);obj=d/(module+'.o');subprocess.run([prefix+'as','-o',str(obj),str(asm)],check=True);objects.append(obj)
   for module in ['syscall','entry']:
    obj=d/(module+'.o');subprocess.run([prefix+'as','-o',str(obj),str(ROOT/('runtime/linux-'+arch+'/'+module+'.S'))],check=True);objects.append(obj)
   exe=d/('program'+str(index));subprocess.run([prefix+'ld','-static','-pie','--no-dynamic-linker','-z','text','-z','noexecstack','--build-id=none','-s','-e','_start','-o',str(exe),*map(str,objects)],check=True)
   env=os.environ.copy();env['LUXRT_STARTUP_TEST']='expected'
   r=subprocess.run(run+[str(exe),'marker','third'],env=env,capture_output=True,timeout=10)
   assert r.returncode==0,(a.target,r)
   expected=f'3\n{os.sysconf("SC_PAGESIZE")}\nmarker\nexpected\n'.encode();assert r.stdout==expected,(r.stdout,expected);assert not r.stderr
   assert old is None or old==exe.read_bytes();old=exe.read_bytes()
   info=subprocess.check_output([prefix+'readelf','-W','-l',str(exe)],text=True);assert 'INTERP' not in info
   stack=next(line for line in info.splitlines() if 'GNU_STACK' in line);assert 'RWE' not in stack
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'target':a.target,'output':expected.decode(),'binary_identical':True,'libc':False,'dynamic_loader':False,'native':not bool(run),'emulator':run},indent=2)+'\n')
 print('real kernel startup test passed:',a.target)
if __name__=='__main__':main()
