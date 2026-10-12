#!/usr/bin/env python3
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
CONF={'amd64_sysv':('cc',[]),'arm64':('aarch64-linux-gnu-gcc',['qemu-aarch64']),'rv64':('riscv64-linux-gnu-gcc',['qemu-riscv64'])}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();cc,runner=CONF[a.target]
 flags=[cc,'-static','-no-pie','-O2','-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/runtime/relocate_driver.c')]
 with tempfile.TemporaryDirectory(prefix='luxrt-relocate-') as d:
  d=Path(d);exe=d/'test';subprocess.run(flags+[str(ROOT/'runtime/relocate.c'),str(ROOT/'runtime/startup.c'),'-o',str(exe)],check=True);expected=subprocess.check_output(runner+[str(exe)],timeout=30);old={}
  for backend in a.backend:
   asm=[]
   for module in ['relocate','startup']:
    r=subprocess.run([str(backend.resolve()),'-t',a.target,str(ROOT/('runtime/'+module+'.lir'))],capture_output=True,timeout=30);assert r.returncode==0,r.stderr;assert module not in old or old[module]==r.stdout;old[module]=r.stdout;p=d/(module+'.s');p.write_bytes(r.stdout);asm.append(p)
   subprocess.run(flags+list(map(str,asm))+['-o',str(exe)],check=True);assert subprocess.check_output(runner+[str(exe)],timeout=30)==expected
  print(a.target,expected.decode().strip())
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'target':a.target,'output':expected.decode().strip(),'assembly_parity':True,'runner':runner},indent=2)+'\n')
if __name__=='__main__':main()
