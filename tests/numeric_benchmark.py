#!/usr/bin/env python3
import argparse,csv,io,json,statistics,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,required=True);ap.add_argument('--report',type=Path,required=True);a=ap.parse_args()
 with tempfile.TemporaryDirectory(prefix='luxir-numeric-bench-') as d:
  d=Path(d);asm=d/'numeric.s';r=subprocess.run([str(a.backend.resolve()),str(ROOT/'tests/numeric.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr;asm.write_bytes(r.stdout);exe=d/'test'
  subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/numeric_benchmark.c'),str(asm),'-o',str(exe)],check=True)
  output=subprocess.check_output([str(exe)],text=True,timeout=60);rows=[{k:float(v) for k,v in row.items()} for row in csv.DictReader(io.StringIO(output))]
  intrinsic=statistics.median(x['intrinsic_seconds'] for x in rows);fallback=statistics.median(x['fallback_seconds'] for x in rows)
  report={'target':'amd64_sysv','backend':str(a.backend),'items':1000000,'repetitions':16,'trials':rows,'intrinsic_median_seconds':intrinsic,'fallback_median_seconds':fallback,'speedup':fallback/intrinsic,'correctness':'every trial checked against uint128 oracle','allocation_and_input_generation_timed':False,'scope':'isolated umulh loop; not whole LSL runtime or compiler'}
  a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
if __name__=='__main__':main()
