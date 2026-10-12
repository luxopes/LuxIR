#!/usr/bin/env python3
import argparse,json,os,statistics,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,required=True);ap.add_argument('--report',type=Path,required=True);args=ap.parse_args();cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu});results={'cpu':cpu,'trials':9,'calls_per_trial':1000000,'modes':{}}
 with tempfile.TemporaryDirectory(prefix='luxrt-perf-') as tmp:
  tmp=Path(tmp);flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror'];driver=ROOT/'tests/runtime/benchmark_driver.c';ref=tmp/'reference';native=tmp/'native'
  subprocess.run(flags+[str(driver),str(ROOT/'runtime/memory.c'),str(ROOT/'runtime/integer.c'),'-o',str(ref)],check=True);assemblies=[]
  for name in ['memory','integer']:
   p=tmp/(name+'.s');r=subprocess.run([str(args.backend.resolve()),str(ROOT/'runtime'/(name+'.lir'))],capture_output=True,timeout=30);assert r.returncode==0,r.stderr;p.write_bytes(r.stdout);assemblies.append(str(p))
  subprocess.run(flags+[str(driver),*assemblies,'-o',str(native)],check=True)
  for mode in ['hash','utf8','format']:
   samples={'reference':[],'native':[]};checksum=None
   for trial in range(9):
    for label in (['reference','native'] if trial%2==0 else ['native','reference']):
     p=ref if label=='reference' else native;out=subprocess.check_output([str(p),mode],timeout=20).decode().strip().split(',');value=int(out[1]);checksum=value if checksum is None else checksum;assert value==checksum;samples[label].append(float(out[0]))
   medians={k:statistics.median(v) for k,v in samples.items()};results['modes'][mode]={'seconds':samples,'medians':medians,'native_over_reference':medians['native']/medians['reference'],'checksum':checksum};print(mode,medians,flush=True)
 args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(results,indent=2))
if __name__=='__main__':main()
