#!/usr/bin/env python3
"""Actual local-exec ELF TLS in 40 raw threads; no libc in worker callbacks."""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
CONF={'amd64_sysv':('x86_64','cc',[]),'arm64':('arm64','aarch64-linux-gnu-gcc',['qemu-aarch64']),'rv64':('rv64','riscv64-linux-gnu-gcc',['qemu-riscv64'])}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args()
 arch,cc,runner=CONF[a.target];platform=ROOT/('runtime/linux-'+arch);flags=[cc,'-static','-no-pie','-O2','-std=c11','-Wall','-Wextra','-Werror','-fno-stack-protector'];base=[ROOT/'tests/runtime/tls_driver.c',*[platform/n for n in ['syscall.S','thread.S','atomic.S','tls.S']]]
 modules=[('tls',ROOT/'runtime/tls.c',ROOT/'runtime/tls.lir'),('memory',ROOT/'runtime/memory.c',ROOT/'runtime/memory.lir'),('startup',ROOT/'runtime/startup.c',ROOT/'runtime/startup.lir'),('os',platform/'os.c',platform/'os.lir'),('thread',platform/'thread.c',platform/'thread.lir')]
 with tempfile.TemporaryDirectory(prefix='luxrt-tls-') as d:
  d=Path(d);exe=d/'test';subprocess.run(flags+list(map(str,base+[m[1] for m in modules]))+['-o',str(exe)],check=True)
  expected=subprocess.check_output(runner+[str(exe)],timeout=60);print(a.target,expected.decode().strip(),flush=True);old={}
  for backend in a.backend:
   asm=[]
   worker=d/'tls-worker.s';r=subprocess.run([str(backend.resolve()),'-t',a.target,str(ROOT/'tests/runtime/tls_worker.lir')],capture_output=True,timeout=30);assert r.returncode==0,r.stderr;worker.write_bytes(r.stdout);asm.append(worker)
   assert 'worker' not in old or old['worker']==r.stdout;old['worker']=r.stdout
   for name,c,source in modules+[('tls-native',None,platform/'tls-native.lir')]:
    r=subprocess.run([str(backend.resolve()),'-t',a.target,str(source)],capture_output=True,timeout=30);assert r.returncode==0,r.stderr
    assert name not in old or old[name]==r.stdout;old[name]=r.stdout;p=d/(name+'.s');p.write_bytes(r.stdout);asm.append(p)
   subprocess.run(flags+['-DLUXRT_IR_WORKER']+list(map(str,base+asm))+['-o',str(exe)],check=True);assert subprocess.check_output(runner+[str(exe)],timeout=60)==expected
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps({'target':a.target,'output':expected.decode().strip(),'assembly_parity':True,'threads':40,'worker_libc':False,'managed_gc':False,'runner':runner},indent=2)+'\n')
if __name__=='__main__':main()
