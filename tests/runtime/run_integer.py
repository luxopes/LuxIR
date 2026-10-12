#!/usr/bin/env python3
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);args=ap.parse_args()
 with tempfile.TemporaryDirectory(prefix='luxrt-integer-') as tmp:
  tmp=Path(tmp);driver=ROOT/'tests/runtime/integer_driver.c';exe=tmp/'reference';flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror']
  subprocess.run(flags+[str(driver),str(ROOT/'runtime/integer.c'),'-o',str(exe)],check=True);expected=subprocess.check_output([str(exe)],timeout=20);print(expected.decode().strip());assemblies={}
  for backend in args.backend:
   for target in TARGETS:
    r=subprocess.run([str(backend.resolve()),'-t',target,str(ROOT/'runtime/integer.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    if target in assemblies:assert r.stdout==assemblies[target]
    assemblies[target]=r.stdout
    if target=='amd64_sysv':
     asm=tmp/'integer.s';asm.write_bytes(r.stdout);subprocess.run(flags+[str(driver),str(asm),'-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],timeout=20)==expected
  subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',str(driver),str(ROOT/'runtime/integer.c'),'-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=30)==expected
  result={'output':expected.decode().strip(),'assembly_parity_targets':TARGETS,'linux_execution':True,'other_targets_execution':False,'c_asan_ubsan':True,'leak_sanitizer':False}
  if args.report:args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(result,indent=2))
 print('decimal integer runtime tests passed')
if __name__=='__main__':main()
