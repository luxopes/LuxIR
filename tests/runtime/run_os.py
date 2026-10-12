#!/usr/bin/env python3
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);args=ap.parse_args()
 with tempfile.TemporaryDirectory(prefix='luxrt-os-') as tmp:
  tmp=Path(tmp);drivers=[ROOT/'tests/runtime/os_driver.c',ROOT/'tests/runtime/os_extended_driver.c'];driver=drivers[0];gate=ROOT/'runtime/linux-x86_64/syscall.S';exe=tmp/'reference';flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror']
  expected=[]
  for driver in drivers:
   subprocess.run(flags+[str(driver),str(ROOT/'runtime/linux-x86_64/os.c'),str(gate),'-o',str(exe)],check=True);expected.append(subprocess.check_output([str(exe)],timeout=10));print(expected[-1].decode().strip())
  assembly=None
  for backend in args.backend:
   r=subprocess.run([str(backend.resolve()),str(ROOT/'runtime/linux-x86_64/os.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
   if assembly is not None:assert r.stdout==assembly
   assembly=r.stdout;asm=tmp/'os.s';asm.write_bytes(assembly)
   for driver,output in zip(drivers,expected):
    subprocess.run(flags+[str(driver),str(asm),str(gate),'-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],timeout=10)==output
  if args.report:args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps({'output':[out.decode().strip() for out in expected],'backends':list(map(str,args.backend)),'assembly_parity':True,'target':'linux-x86_64','libc_calls_in_runtime':False},indent=2))
 print('Linux syscall runtime tests passed')
if __name__=='__main__':main()
