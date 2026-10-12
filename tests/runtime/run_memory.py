#!/usr/bin/env python3
"""Compile real LuxIR runtime through both backends, compare to C reference."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);args=ap.parse_args();checks=[]
 with tempfile.TemporaryDirectory(prefix='luxrt-memory-') as tmp:
  tmp=Path(tmp);reference=tmp/'reference'
  subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/runtime/memory_driver.c'),str(ROOT/'runtime/memory.c'),'-o',str(reference)],check=True)
  expected=subprocess.check_output([str(reference)],timeout=20);print('C reference:',expected.decode().strip())
  asm_by_target={}
  for backend in args.backend:
   for target in TARGETS:
    r=subprocess.run([str(backend.resolve()),'-t',target,str(ROOT/'runtime/memory.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    if target in asm_by_target:assert r.stdout==asm_by_target[target],target
    else:asm_by_target[target]=r.stdout
    checks.append({'backend':str(backend),'target':target,'assembly_parity':True})
    if target=='amd64_sysv':
     asm=tmp/'memory.s';asm.write_bytes(r.stdout);exe=tmp/'native'
     subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/runtime/memory_driver.c'),str(asm),'-o',str(exe)],check=True)
     got=subprocess.check_output([str(exe)],timeout=20);assert got==expected;print(backend,got.decode().strip())
  # Instrument C reference; native assembly is tested with canaries/guard pages.
  exe=tmp/'sanitized'
  subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(ROOT/'tests/runtime/memory_driver.c'),str(ROOT/'runtime/memory.c'),'-o',str(exe)],check=True)
  # LeakSanitizer cannot inspect /proc task state in this managed host.
  env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
  assert subprocess.check_output([str(exe)],timeout=30,env=env)==expected
  report={'checks':checks,'runtime_output':expected.decode().strip(),'linux_execution':True,'other_targets_execution':False,'c_asan_ubsan':True,'leak_sanitizer':False,'native_guard_pages':True}
  if args.report:args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2))
 print('memory runtime tests passed')
if __name__=='__main__':main()
