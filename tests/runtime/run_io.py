#!/usr/bin/env python3
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);a=ap.parse_args()
 flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror'];base=[ROOT/'tests/runtime/io_driver.c',ROOT/'runtime/linux-x86_64/syscall.S']
 with tempfile.TemporaryDirectory(prefix='luxrt-io-') as d:
  d=Path(d);exe=d/'test';subprocess.run(flags+list(map(str,base))+[str(ROOT/'runtime/io.c'),str(ROOT/'runtime/linux-x86_64/os.c'),'-o',str(exe)],check=True)
  expected=subprocess.check_output([str(exe)],timeout=15);print(expected.decode().strip());old={}
  for backend in a.backend:
   assembly=[]
   for name,path in [('io','runtime/io.lir'),('os','runtime/linux-x86_64/os.lir')]:
    r=subprocess.run([str(backend.resolve()),str(ROOT/path)],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    assert name not in old or old[name]==r.stdout;old[name]=r.stdout;out=d/(name+'.s');out.write_bytes(r.stdout);assembly.append(out)
   subprocess.run(flags+list(map(str,base+assembly))+['-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],timeout=15)==expected
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'output':expected.decode().strip(),'assembly_parity':True,'target':'linux-x86_64'},indent=2)+'\n')
if __name__=='__main__':main()
