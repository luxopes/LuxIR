#!/usr/bin/env python3
"""Build an actual static PIE luxrt executable, without libc or ELF loader."""
import argparse,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);args=ap.parse_args();checks=[]
 with tempfile.TemporaryDirectory(prefix='luxrt-standalone-') as tmp:
  tmp=Path(tmp);old=None
  for index,backend in enumerate(args.backend):
   objects=[]
   for name,path in [('demo',ROOT/'examples/runtime-demo.lir'),('integer',ROOT/'runtime/integer.lir'),('os',ROOT/'runtime/linux-x86_64/os.lir')]:
    assembly=tmp/(name+'.s');r=subprocess.run([str(backend.resolve()),str(path)],capture_output=True,timeout=30);assert r.returncode==0,r.stderr;assembly.write_bytes(r.stdout);obj=tmp/(name+'.o');subprocess.run(['as','--64','-o',str(obj),str(assembly)],check=True);objects.append(obj)
   for name in ['syscall','start']:
    obj=tmp/(name+'.o');subprocess.run(['as','--64','-o',str(obj),str(ROOT/'runtime/linux-x86_64'/(name+'.S'))],check=True);objects.append(obj)
   exe=tmp/('standalone-'+str(index));subprocess.run(['ld','-nostdlib','-static','-pie','--no-dynamic-linker','-z','text','-z','noexecstack','--build-id=none','-s','-e','_start','-o',str(exe),*map(str,objects)],check=True)
   r=subprocess.run([str(exe)],capture_output=True,timeout=5);assert (r.returncode,r.stdout,r.stderr)==(0,b'9223372036854775807\n',b'')
   info=subprocess.check_output(['readelf','-W','-l',str(exe)],text=True);assert 'INTERP' not in info
   stack=next(line for line in info.splitlines() if 'GNU_STACK' in line);assert ' E ' not in stack and 'RWE' not in stack
   header=subprocess.check_output(['readelf','-h',str(exe)],text=True);assert 'DYN' in header
   image=exe.read_bytes();assert old is None or old==image;old=image
   checks.append({'backend':str(backend),'bytes':len(image),'static_pie':True,'libc':False,'dynamic_loader':False,'nonexecutable_stack':True,'native_execution':True})
  result={'checks':checks,'binary_identical':True,'assembler':'GNU as','linker':'GNU ld','own_assembler_linker':False}
  if args.report:args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(result,indent=2))
 print('standalone static PIE runtime tests passed')
if __name__=='__main__':main()
