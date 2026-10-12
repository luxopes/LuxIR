#!/usr/bin/env python3
"""Exact graph reachability oracle, scalar fidelity, roots and raw Linux GC."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
CONF={'amd64_sysv':[],'arm64':['qemu-aarch64'],'rv64':['qemu-riscv64']}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();assemblies={}
 with tempfile.TemporaryDirectory(prefix='luxrt-managed-') as d:
  d=Path(d);driver=ROOT/'tests/runtime/managed_driver.c';shim=ROOT/'tests/runtime/managed_os_oracle.c';memory=ROOT/'runtime/memory.c';exe=d/'oracle';flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror']
  subprocess.run(flags+[str(driver),str(shim),str(memory),str(ROOT/'runtime/managed.c'),'-o',str(exe)],check=True);expected=subprocess.check_output([str(exe)],timeout=30);print(expected.decode().strip(),flush=True);previous=None
  for index,b in enumerate(a.backend):
   for target in TARGETS:
    asm=subprocess.check_output([str(b.resolve()),'-t',target,str(ROOT/'runtime/managed.lir')],timeout=30)
    if target in assemblies:assert asm==assemblies[target]
    assemblies[target]=asm
    if target=='amd64_sysv':
     p=d/'managed.s';p.write_bytes(asm);subprocess.run(flags+[str(driver),str(shim),str(memory),str(p),'-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],timeout=30)==expected
   binary=d/('raw'+str(index));subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(b.resolve()),'-t',a.target,'-o',str(binary),str(ROOT/'tests/runtime/managed_smoke.lir'),str(ROOT/'runtime/managed.lir')],check=True,timeout=60);r=subprocess.run(CONF[a.target]+[str(binary)],capture_output=True,timeout=20);assert r.returncode==0 and not r.stdout and not r.stderr,r
   blob=binary.read_bytes();assert previous is None or previous==blob;previous=blob
  subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',str(driver),str(shim),str(memory),str(ROOT/'runtime/managed.c'),'-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=30)==expected
  report={'target':a.target,'oracle':expected.decode().strip(),'assembly_parity_targets':TARGETS,'raw_binary_identical':True,'c_asan_ubsan':True,'leak_sanitizer':False,'all_mappings_released':True,'exact_graph_oracle':True,'deep_chain_nodes':30000,'managed_mode':'single-owner explicit-roots non-moving mark-sweep','runner':CONF[a.target]}
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
  print(json.dumps(report))
if __name__=='__main__':main()
