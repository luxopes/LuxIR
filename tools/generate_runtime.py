#!/usr/bin/env python3
"""Generate freestanding LP64 runtime IR from C reference, without libc.

Developer-only cproc frontend -> QBE IL -> LuxIR. Committed IR needs neither
cproc nor a C compiler at build/run time. Fail on unsupported constructs.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
from qbe2lir import convert
ROOT=Path(__file__).resolve().parents[1]
CPROC_COMMIT='d1c53ddf56571573a7025324c8dd5c6d547a4d1f'
def generate(source,cproc):
 pp=subprocess.check_output(['cc','-E','-P','-std=c11','-nostdinc','-I',str(ROOT/'tools/runtime-include'),str(source.resolve())])
 ssa=subprocess.check_output([str(cproc.resolve()),'-t','x86_64-sysv'],input=pp)
 result='ir_version "1"\nruntime_abi "none"\n'+convert(ssa.decode())
 # These modules use only fixed-width scalar and pointer ABI. Varargs,
 # bitfields and ABI-dependent aggregate parameters are not allowed.
 if '...' in result:raise ValueError('variadic runtime definition')
 return result

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--cproc',type=Path,required=True);ap.add_argument('--check',action='store_true');ap.add_argument('--report',type=Path);ap.add_argument('sources',type=Path,nargs='+');a=ap.parse_args();rows=[]
 for source in a.sources:
  result=generate(source,a.cproc);output=source.with_suffix('.lir')
  if a.check:
   if not output.exists() or output.read_text()!=result:raise SystemExit('stale generated IR: '+str(output))
  else:output.write_text(result)
  rows.append({'source':str(source),'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'ir_sha256':hashlib.sha256(result.encode()).hexdigest()})
 report={'cproc_source_commit':CPROC_COMMIT,'abi':'LP64 scalar/pointer, no varargs','checked_only':a.check,'modules':rows}
 if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
