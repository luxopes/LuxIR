#!/usr/bin/env python3
"""Lookup of distinct equal cached strings; no allocation/GC in timed loops."""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
MODULES=['managed','collections','map']
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);ap.add_argument('--inline',action='store_true');a=ap.parse_args();rows=[]
 with tempfile.TemporaryDirectory(prefix='luxrt-map-bench-') as d:
  d=Path(d);common=[ROOT/'tests/runtime/map_benchmark.c',ROOT/'tests/runtime/managed_os_oracle.c',ROOT/'runtime/memory.c'];variants=[('C reference',[ROOT/('runtime/'+m+'.c') for m in MODULES])]
  for i,b in enumerate(a.backend):
   sources=[]
   for m in MODULES:
    p=d/(m+str(i)+'.s');p.write_bytes(subprocess.check_output([str(b.resolve())]+(['-i'] if a.inline else [])+['-t','amd64_sysv',str(ROOT/('runtime/'+m+'.lir'))]));sources.append(p)
   variants.append((str(b),sources))
  for i,(name,sources) in enumerate(variants):
   exe=d/str(i);subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror']+list(map(str,common+sources))+['-o',str(exe)],check=True);r=json.loads(subprocess.check_output([str(exe)],timeout=30));r['implementation']=name;rows.append(r)
 report={'target':'amd64_sysv','results':rows,'inline':a.inline,'excludes':['allocation','GC','string construction'],'includes':['function calls','hash/probe','full byte equality','checksum'],'caveat':'isolated lookup, not complete contest programs'}
 if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
