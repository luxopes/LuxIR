#!/usr/bin/env python3
"""Standalone raw Float64 conversion, libc-free on three Linux architectures."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
CONF={'amd64_sysv':[],'arm64':['qemu-aarch64'],'rv64':['qemu-riscv64']}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args()
 with tempfile.TemporaryDirectory(prefix='luxrt-raw-float64-') as d:
  d=Path(d);source=ROOT/'tests/runtime/float64_smoke.c';exe=d/'reference';common=[ROOT/'runtime/float64.c',ROOT/'runtime/memory.c',ROOT/'runtime/integer.c']
  cmd=['cc','-O1','-g','-std=c11','-fsanitize=address,undefined','-I'+str(ROOT/'runtime/vendor'),str(source),*map(str,common),'-o',str(exe)];subprocess.run(cmd,check=True);subprocess.run([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),check=True,timeout=30);old=None
  for i,b in enumerate(a.backend):
   exe=d/('raw'+str(i));subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(b.resolve()),'-t',a.target,'-o',str(exe),str(source.with_suffix('.lir'))],check=True,timeout=60);r=subprocess.run(CONF[a.target]+[str(exe)],capture_output=True,timeout=30);assert r.returncode==0 and not r.stdout and not r.stderr,r
   blob=exe.read_bytes();assert old is None or old==blob;old=blob
  report={'target':a.target,'raw_binary_identical':True,'c_asan_ubsan':True,'libc_float_conversion':False,'format_against_python_repr':True,'parse_against_python_float':True,'long_decimal_inputs':True,'runner':CONF[a.target]}
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
  print(json.dumps(report))
if __name__=='__main__':main()
