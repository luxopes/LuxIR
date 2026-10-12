#!/usr/bin/env python3
"""Run callbacks on guarded raw clone stacks and join via clear-child-tid futex."""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);a=ap.parse_args()
 flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-fno-stack-protector'];sources=[ROOT/'tests/runtime/thread_driver.c',ROOT/'runtime/linux-x86_64/thread.S',ROOT/'runtime/linux-x86_64/syscall.S',ROOT/'runtime/linux-x86_64/atomic.S']
 with tempfile.TemporaryDirectory(prefix='luxrt-thread-') as d:
  d=Path(d);exe=d/'test';subprocess.run(flags+list(map(str,sources))+[str(ROOT/'runtime/linux-x86_64/thread.c'),str(ROOT/'runtime/linux-x86_64/os.c'),'-o',str(exe)],check=True)
  expected=subprocess.check_output([str(exe)],timeout=30);print(expected.decode().strip());assembly={}
  for backend in a.backend:
   asm=[]
   for module in ['thread','os']:
    r=subprocess.run([str(backend.resolve()),str(ROOT/('runtime/linux-x86_64/'+module+'.lir'))],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    assert module not in assembly or assembly[module]==r.stdout;assembly[module]=r.stdout;p=d/(module+'.s');p.write_bytes(r.stdout);asm.append(p)
   subprocess.run(flags+list(map(str,sources+asm))+['-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],timeout=30)==expected
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'output':expected.decode().strip(),'assembly_parity':True,'target':'linux-x86_64','tls':False,'managed_gc':False},indent=2)+'\n')
if __name__=='__main__':main()
