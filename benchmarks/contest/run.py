#!/usr/bin/env python3
"""Controlled opera kernels: raw Int64 matrix vs allocation-free text scan.

Same data/algorithm/driver. This isolates backend code, not LSL split/Any/GC
or filesystem input. C -O3-native is a separate SIMD-friendly target, never
silently mixed into the baseline. Preparation is outside the timer.
"""
import argparse,json,os,re,statistics,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path,default=ROOT/'build/opera-benchmark.json');ap.add_argument('--rows',type=int,default=100000);ap.add_argument('--columns',type=int,default=100);ap.add_argument('--trials',type=int,default=7);a=ap.parse_args()
 cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu});result={'cpu':cpu,'rows':a.rows,'columns':a.columns,'elements':a.rows*a.columns,'trials':a.trials,'repetitions_per_trial':{'numeric':16,'text':4},'lsl_end_to_end':False,'parity_targets':TARGETS,'modes':{},'disassembly':{}}
 with tempfile.TemporaryDirectory(prefix='luxir-opera-') as d:
  d=Path(d);asm=d/'opera.s'
  for target in TARGETS:
   baseline=None
   for backend in a.backend:
    r=subprocess.run([str(backend.resolve()),'-t',target,str(HERE/'opera.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    assert baseline is None or baseline==r.stdout,(target,backend);baseline=r.stdout
   if target=='amd64_sysv':asm.write_bytes(baseline)
  bins={};flags=['cc','-std=c11','-Wall','-Wextra','-Werror']
  driver=d/'driver.o';subprocess.run(flags+['-O2','-c',str(HERE/'driver.c'),'-o',str(driver)],check=True)
  variants={'C-O2':['-O2'],'C-O3':['-O3'],'C-O3-native':['-O3','-march=native'],'LuxIR':[]}
  for label,opt in variants.items():
   obj=d/(label+'.o');exe=d/label;source=asm if label=='LuxIR' else HERE/'opera.c'
   subprocess.run(flags+opt+['-c',str(source),'-o',str(obj)],check=True);subprocess.run(['cc',str(driver),str(obj),'-o',str(exe)],check=True);bins[label]=exe
   test=d/'check';subprocess.run(flags+['-O2',str(HERE/'checks.c'),str(obj),'-o',str(test)],check=True);out=subprocess.check_output([str(test)],timeout=10)
   dis=subprocess.check_output(['objdump','-d','--disassemble=opera_numeric',str(obj)],text=True)
   result['disassembly'][label]={'vector_registers':bool(re.search(r'%[xyz]mm',dis)),'text':dis}
  result['correctness']=out.decode().strip()
  for mode in ['numeric','text']:
   samples={k:[] for k in bins};checksum=None
   for trial in range(a.trials):
    order=list(bins)
    if trial%2:order.reverse()
    for label in order:
     output=subprocess.check_output([str(bins[label]),mode,str(a.rows),str(a.columns)],timeout=60).decode().strip().split(',')
     value=(int(output[1]),int(output[2]));assert checksum is None or checksum==value;checksum=value;samples[label].append(float(output[0]))
   medians={k:statistics.median(v) for k,v in samples.items()}
   result['modes'][mode]={'samples_seconds':samples,'medians_seconds':medians,'checksum':checksum[0],'text_bytes':checksum[1],'LuxIR_over_C_O2':medians['LuxIR']/medians['C-O2'],'LuxIR_over_C_O3_native':medians['LuxIR']/medians['C-O3-native']}
   print(mode,json.dumps(result['modes'][mode]),flush=True)
  a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(result,indent=2)+'\n')
 print(result['correctness'])
if __name__=='__main__':main()
