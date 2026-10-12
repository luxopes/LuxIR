#!/usr/bin/env python3
"""C11 atomic oracle vs architecture gate; thread contention and SC litmus."""
import json,subprocess,tempfile,argparse
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--report',type=Path);a=ap.parse_args();outputs=[]
 with tempfile.TemporaryDirectory(prefix='luxrt-atomic-') as d:
  exe=Path(d)/'test'
  for source in [ROOT/'runtime/atomic.c',ROOT/'runtime/linux-x86_64/atomic.S']:
   subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-pthread',str(ROOT/'tests/runtime/atomic_driver.c'),str(source),'-o',str(exe)],check=True)
   outputs.append(subprocess.check_output([str(exe)],timeout=30))
  assert outputs[0]==outputs[1];print(outputs[0].decode().strip())
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'output':outputs[0].decode().strip(),'execution_targets':['linux-x86_64'],'arm64_rv64_execution':False,'backend_ir_instructions':False},indent=2)+'\n')
if __name__=='__main__':main()
