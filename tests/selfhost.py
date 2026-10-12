#!/usr/bin/env python3
"""Two self-host generations, binary fixed point and an explicit wall-time gate.

Existing LSL compiler is a toolchain input, never modified. It may need its
own resource working directory. A deadline failure leaves logs for diagnosis
and never counts as successful self-host. The limit is machine-specific.
"""
import argparse,hashlib,json,subprocess,time,os,signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--compiler',type=Path,required=True);ap.add_argument('--backend',type=Path,required=True);ap.add_argument('--compiler-cwd',type=Path,default=ROOT);ap.add_argument('--limit',type=float,default=60);ap.add_argument('--report',type=Path,default=ROOT/'build/selfhost.json');a=ap.parse_args()
 if a.limit<=0:ap.error('--limit must be positive')
 directory=ROOT/'build/selfhost';directory.mkdir(parents=True,exist_ok=True);rows=[];backend=a.backend.resolve();initial=sha(backend)
 for generation in [2,3]:
  output=directory/('luxir-stage'+str(generation));log=directory/('stage'+str(generation)+'.log');cmd=[str(a.compiler.resolve()),str(ROOT/'lsl/luxir.lsl'),str(output),str(backend)];start=time.monotonic()
  with log.open('wb') as stream:
   process=subprocess.Popen(cmd,cwd=a.compiler_cwd.resolve(),stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
   try:code=process.wait(timeout=a.limit)
   except subprocess.TimeoutExpired:
    os.killpg(process.pid,signal.SIGKILL);process.wait();code='timeout'
  elapsed=time.monotonic()-start
  row={'generation':generation,'seconds':elapsed,'limit_seconds':a.limit,'exit_code':code,'backend':str(backend),'log':str(log)}
  if code==0:row['sha256']=sha(output)
  rows.append(row);result={'initial_sha256':initial,'generations':rows,'complete':generation==3 and code==0,'fixed_point':generation==3 and code==0 and all(x.get('sha256')==initial for x in rows),'time_gate_passed':generation==3 and all(x['exit_code']==0 and x['seconds']<=a.limit for x in rows),'target':'amd64_sysv'}
  a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(result,indent=2)+'\n')
  print(json.dumps(row),flush=True)
  if code!=0:raise SystemExit('self-host failed: '+str(log))
  if row['sha256']!=initial:raise SystemExit('self-host binary is not a fixed point')
  backend=output
 print('self-host: stage 1 == stage 2 == stage 3; both compilations inside deadline')
if __name__=='__main__':main()
