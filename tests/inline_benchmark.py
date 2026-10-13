#!/usr/bin/env python3
"""Isolate a scalar helper call inside a native loop; exclude Python calls."""
import argparse,ctypes,json,statistics,subprocess,tempfile,time
from pathlib import Path
from inline import SSA,convert,oracle
LOOP='''export function l $kernel(l %seed, l %n) {
@start
 jmp @loop
@loop
 %i =l phi @start 0, @loop %next
 %v =l phi @start %seed, @loop %w
 %w =l call $mix(l %v, l %i)
 %next =l add %i, 1
 %more =w cultl %next, %n
 jnz %more, @loop, @done
@done
 ret %w
}
'''
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);a=ap.parse_args();rows=[]
 n=2_000_000;expected=123
 for i in range(n):expected=oracle(expected,i)
 with tempfile.TemporaryDirectory(prefix='luxir-inl-bench-') as d:
  d=Path(d);lir=d/'bench.lir';lir.write_text(convert(SSA.split('export function l $twice')[0]+LOOP))
  for b in a.backend:
   for enabled in (False,True):
    asm=d/'bench.s';asm.write_bytes(subprocess.check_output([str(b.resolve())]+(['-i'] if enabled else [])+[str(lir)]));so=d/(str(len(rows))+'.so');subprocess.run(['cc','-shared','-Wl,-Bsymbolic',str(asm),'-o',str(so)],check=True)
    lib=ctypes.CDLL(str(so));f=lib.kernel;f.argtypes=[ctypes.c_uint64,ctypes.c_uint64];f.restype=ctypes.c_uint64
    assert f(123,n)==expected
    times=[]
    for _ in range(9):
     start=time.perf_counter_ns();result=f(123,n);elapsed=time.perf_counter_ns()-start;assert result==expected;times.append(elapsed/n)
    rows.append({'backend':str(b),'inline':enabled,'iterations':n,'trials':9,'median_ns_per_iteration':statistics.median(times),'checksum':expected})
 report={'results':rows,'excludes':['Python loop','allocation','GC','I/O'],'caveat':'scalar helper microbenchmark; not an end-to-end LSL workload'}
 if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
