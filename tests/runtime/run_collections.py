#!/usr/bin/env python3
"""Managed collection models, GC stress, sanitizers and raw Linux execution."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
CONF={'amd64_sysv':[],'arm64':['qemu-aarch64'],'rv64':['qemu-riscv64']}
MODULES=['managed','collections','map']
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();assemblies={}
 with tempfile.TemporaryDirectory(prefix='luxrt-collections-') as d:
  d=Path(d);common=[ROOT/'tests/runtime/collections_driver.c',ROOT/'tests/runtime/managed_os_oracle.c',ROOT/'runtime/memory.c'];sources=[ROOT/('runtime/'+m+'.c') for m in MODULES];exe=d/'oracle';flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror']
  subprocess.run(flags+list(map(str,common+sources))+['-o',str(exe)],check=True);expected=subprocess.check_output([str(exe)],timeout=30);print(expected.decode().strip(),flush=True);previous=None
  for i,b in enumerate(a.backend):
   host=[]
   for module in MODULES:
    for target in TARGETS:
     asm=subprocess.check_output([str(b.resolve()),'-t',target,str(ROOT/('runtime/'+module+'.lir'))],timeout=30);key=(module,target)
     if key in assemblies:assert asm==assemblies[key]
     assemblies[key]=asm
     if target=='amd64_sysv':p=d/(module+'.s');p.write_bytes(asm);host.append(p)
   subprocess.run(flags+list(map(str,common+host))+['-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],timeout=30)==expected
   binary=d/('raw'+str(i));subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--managed','--backend',str(b.resolve()),'-t',a.target,'-o',str(binary),str(ROOT/'tests/runtime/collections_smoke.lir')],check=True,timeout=60);r=subprocess.run(CONF[a.target]+[str(binary)],capture_output=True,timeout=20);assert r.returncode==0 and not r.stdout and not r.stderr,r
   blob=binary.read_bytes();assert previous is None or previous==blob;previous=blob
  subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined']+list(map(str,common+sources))+['-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=45)==expected
  report={'target':a.target,'oracle':expected.decode().strip(),'assembly_parity_targets':TARGETS,'raw_binary_identical':True,'c_asan_ubsan':True,'leak_sanitizer':False,'all_mappings_released':True,'models':['UTF-8 codepoints and byte spans','dynamic Value list','raw Int64/Float64/Byte arrays','insertion-ordered map','numeric key equivalence','NaN identity','set'],'runner':CONF[a.target]}
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
  print(json.dumps(report))
if __name__=='__main__':main()
