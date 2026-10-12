#!/usr/bin/env python3
"""Actual IR-to-static-PIE build, relocations, TLS layouts, RELRO and DCE."""
import argparse,hashlib,json,re,resource,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CONF={'amd64_sysv':('',[]),'arm64':('aarch64-linux-gnu-',['qemu-aarch64']),'rv64':('riscv64-linux-gnu-',['qemu-riscv64'])}
def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();prefix,runner=CONF[a.target];rows=[];checks=0
 with tempfile.TemporaryDirectory(prefix='luxir-build-test-') as d:
  d=Path(d)
  def build(backend,source,name,keep=False):
   exe=d/name;cmd=['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(backend.resolve()),'-t',a.target,'-o',str(exe),str(source)]
   if keep:cmd+=['--keep-runtime']
   subprocess.run(cmd,check=True,timeout=60);return exe
  old=None
  for index,backend in enumerate(a.backend):
   exe=build(backend,ROOT/'examples/build-demo.lir','demo'+str(index));r=subprocess.run(runner+[str(exe),'marker'],capture_output=True,timeout=10);assert r.returncode==0 and r.stdout==b'42\n' and not r.stderr,(a.target,r);checks+=1
   binary=exe.read_bytes();assert old is None or old==binary;old=binary;info=subprocess.check_output([prefix+'readelf','-W','-l',str(exe)],text=True);assert 'INTERP' not in info and 'GNU_RELRO' in info;assert 'RWE' not in next(line for line in info.splitlines() if 'GNU_STACK' in line);checks+=3
   full=build(backend,ROOT/'examples/build-demo.lir','full'+str(index),True);r=subprocess.run(runner+[str(full)],capture_output=True,timeout=10);assert r.returncode==0 and r.stdout==b'42\n';
   def textsize(path):
    info=subprocess.check_output([prefix+'readelf','-W','-S',str(path)],text=True);return int(re.search(r'\.text\s+PROGBITS\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)',info).group(1),16)
   smalltext=textsize(exe);fulltext=textsize(full);assert fulltext>smalltext;checks+=2
   rows.append({'backend':str(backend),'sha256':hashlib.sha256(binary).hexdigest(),'bytes':len(binary),'bytes_without_gc_sections':full.stat().st_size,'text_bytes':smalltext,'text_bytes_without_gc_sections':fulltext})
   for align in [1,2,4,8,16,32,64]:
    source=d/'tls.lir';source.write_text(f'thread data value align {align} = Int8(41)\nthread data padding align {align} = zero(3)\nexport function main() -> Int32:\n    Int32(v) = load_u8(thread(value))\n    Int32(z) = load_u8(thread(padding))\n    Int32(ok) = eq(v, 41)\n    Int32(zero) = eq(z, 0)\n    Int32(ok) = and(ok, zero)\n    Int32(status) = xor(ok, 1)\n    return status\nend\n')
    exe=build(backend,source,'tls'+str(index)+'-'+str(align));r=subprocess.run(runner+[str(exe)],capture_output=True,timeout=10);assert r.returncode==0,(a.target,align,r);checks+=1
   source=d/'relro.lir';source.write_text('export function main() -> Int32:\n    store(Int64(0), Ptr(_DYNAMIC))\n    return 0\nend\n');exe=build(backend,source,'relro'+str(index));r=subprocess.run(runner+[str(exe)],capture_output=True,preexec_fn=no_core,timeout=10);assert r.returncode in [-11,139],(a.target,r);checks+=1
   # A failed compilation must preserve a previously created output.
   source.write_text('this is invalid IR\n');saved=exe.read_bytes();r=subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(backend.resolve()),'-t',a.target,'-o',str(exe),str(source)],capture_output=True,timeout=30);assert r.returncode!=0 and exe.read_bytes()==saved;checks+=1
  report={'target':a.target,'checks':checks,'backends':rows,'binary_identical':True,'small_tls_alignments':[1,2,4,8,16,32,64],'relative_data_and_tls_pointers':True,'relro_write_fault':True,'failed_build_preserves_output':True,'libc':False,'dynamic_loader':False,'external_tools':['GNU as','GNU ld'],'runner':runner}
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
  print(json.dumps(report))
if __name__=='__main__':main()
