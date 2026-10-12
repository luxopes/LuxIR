#!/usr/bin/env python3
"""Isolated caller-owned arena allocation; excludes OS mapping and managed GC."""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);a=ap.parse_args();rows=[]
 with tempfile.TemporaryDirectory(prefix='luxrt-arena-bench-') as d:
  d=Path(d);driver=ROOT/'tests/runtime/arena_benchmark.c'
  sources=[('C reference',ROOT/'runtime/arena.c')]
  for i,b in enumerate(a.backend):
   p=d/('arena'+str(i)+'.s');p.write_bytes(subprocess.check_output([str(b.resolve()),'-t','amd64_sysv',str(ROOT/'runtime/arena.lir')]));sources.append((str(b),p))
  for i,(name,source) in enumerate(sources):
   exe=d/str(i);subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',str(driver),str(source),'-o',str(exe)],check=True)
   result=json.loads(subprocess.check_output([str(exe)],timeout=30));result['implementation']=name;rows.append(result)
 report={'target':'amd64_sysv','results':rows,'includes':['function_call','alignment_and_capacity_checks','pointer_checksum'],'excludes':['OS_mapping','payload_initialization','object_headers','GC','multi_thread_synchronization']}
 if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
