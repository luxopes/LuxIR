#!/usr/bin/env python3
"""Raw signal return and conservative register snapshot execution on Linux."""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
CONF={'amd64_sysv':('x86_64','cc',[]),'arm64':('arm64','aarch64-linux-gnu-gcc',['qemu-aarch64']),'rv64':('rv64','riscv64-linux-gnu-gcc',['qemu-riscv64'])}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args()
 arch,cc,runner=CONF[a.target];platform=ROOT/('runtime/linux-'+arch);flags=[cc,'-static','-no-pie','-O2','-std=c11','-Wall','-Wextra','-Werror','-fno-stack-protector'];results={}
 with tempfile.TemporaryDirectory(prefix='luxrt-platform-') as d:
  d=Path(d)
  for module in ['signal','context']:
   common=[ROOT/('tests/runtime/'+module+'_driver.c'),platform/'syscall.S',platform/(module+'.S')]
   if module=='context':common.append(ROOT/('tests/runtime/context-'+arch+'.S'))
   reference=platform/('signal.c' if module=='signal' else 'os.c');source=reference.with_suffix('.lir');exe=d/module
   subprocess.run(flags+list(map(str,common+[reference]))+['-o',str(exe)],check=True)
   expected=subprocess.check_output(runner+[str(exe)],timeout=30);print(a.target,expected.decode().strip(),flush=True);assembly={}
   for backend in a.backend:
    asm=[]
    sources=[source]+([ROOT/'tests/runtime/signal_handler.lir'] if module=='signal' else [])
    for n,ir in enumerate(sources):
     r=subprocess.run([str(backend.resolve()),'-t',a.target,str(ir)],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
     assert n not in assembly or assembly[n]==r.stdout;assembly[n]=r.stdout;p=d/(module+str(n)+'.s');p.write_bytes(r.stdout);asm.append(p)
    subprocess.run(flags+(['-DLUXRT_IR_HANDLER'] if module=='signal' else [])+list(map(str,common+asm))+['-o',str(exe)],check=True)
    assert subprocess.check_output(runner+[str(exe)],timeout=30)==expected
   results[module]=expected.decode().strip()
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'target':a.target,'results':results,'assembly_parity':True,'runner':runner,'production_gc':False},indent=2)+'\n')
if __name__=='__main__':main()
