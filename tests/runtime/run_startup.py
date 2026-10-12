#!/usr/bin/env python3
"""Independent synthetic kernel-stack cases, both backends, all target emitters."""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);a=ap.parse_args()
 flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror'];driver=ROOT/'tests/runtime/startup_driver.c'
 with tempfile.TemporaryDirectory(prefix='luxrt-startup-') as d:
  d=Path(d);exe=d/'test';subprocess.run(flags+[str(driver),str(ROOT/'runtime/startup.c'),'-o',str(exe)],check=True)
  expected=subprocess.check_output([str(exe)],timeout=10);print(expected.decode().strip())
  for target in TARGETS:
   old=None
   for backend in a.backend:
    r=subprocess.run([str(backend.resolve()),'-t',target,str(ROOT/'runtime/startup.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    assert old is None or old==r.stdout,(target,backend);old=r.stdout
    if target=='amd64_sysv':
     asm=d/'startup.s';asm.write_bytes(r.stdout);subprocess.run(flags+[str(driver),str(asm),'-o',str(exe)],check=True)
     assert subprocess.check_output([str(exe)],timeout=10)==expected
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'output':expected.decode().strip(),'targets':TARGETS,'assembly_parity':True,'execution_targets':['amd64_sysv']},indent=2)+'\n')
 print('startup runtime tests passed')
if __name__=='__main__':main()
