#!/usr/bin/env python3
"""Alternate baseline/new backend on the same generated, nontrivial IR."""
import argparse, hashlib, json, os, statistics, subprocess, tempfile, time
from pathlib import Path

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline',type=Path,required=True)
    ap.add_argument('--backend',type=Path,required=True)
    ap.add_argument('--report',type=Path,required=True)
    ap.add_argument('--trials',type=int,default=11)
    args=ap.parse_args();cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
    with tempfile.TemporaryDirectory(prefix='luxir-bench-') as tmp:
        source=Path(tmp)/'workload.lir'
        text=[]
        for fn in range(1000):
            text.append(f'export function function{fn}(Int64(x)) -> Int64:\n')
            for i in range(40):
                operand='x' if i==0 else f'v{i-1}'
                text.append(f'    Int64(v{i}) = add({operand}, {i+1})\n')
            text.append('    return v39\nend\n')
        source.write_text(''.join(text));bins={'baseline':args.baseline.resolve(),'new':args.backend.resolve()};times={k:[] for k in bins}
        def run(k):
            start=time.monotonic();r=subprocess.run([str(bins[k]),str(source)],capture_output=True,timeout=60)
            assert r.returncode==0,r.stderr
            return r.stdout,time.monotonic()-start
        expected,_=run('baseline');actual,_=run('new');assert expected==actual,'assembler changed'
        for trial in range(args.trials):
            for k in (['baseline','new'] if trial%2==0 else ['new','baseline']):
                out,elapsed=run(k);assert out==expected;times[k].append(elapsed)
        result={'trials':args.trials,'pinned_cpu':cpu,'input_bytes':source.stat().st_size,'functions':1000,'instructions_per_function':40,'output_sha256':hashlib.sha256(expected).hexdigest(),'assembler_identical':True,'seconds':times,'medians':{k:statistics.median(v) for k,v in times.items()}}
        result['ratio_new_baseline']=result['medians']['new']/result['medians']['baseline'];args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='seconds'}))
if __name__=='__main__':main()
