#!/usr/bin/env python3
"""Independent allocation oracle, canaries, sanitizers and raw Linux execution."""
import argparse,json,os,random,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
CONF={'amd64_sysv':[],'arm64':['qemu-aarch64'],'rv64':['qemu-riscv64']}
def fixture():
 # Static backing data has known 64-byte alignment. Expected cursor offsets
 # are computed in Python, independently of C/IR allocator implementations.
 lines=['data backing align 64 = zero(2048)','export function main() -> Int32:', '    Ptr(arena) = alloc8(24)','    Ptr(out) = alloc8(8)','    Ptr(base) = add(backing, 3)','    Int32(rc) = call luxrt_arena_init(arena, base, Int64(2045))','    Int32(ok) = eq(rc, 0)','    Ptr(used_slot) = add(arena, 16)']
 rng=random.Random(90314);used=0
 for i in range(256):
  size=rng.randrange(0,65);align=1<<rng.randrange(0,7)
  if i%13==0:align=3
  if i%19==0:size=(1<<64)-1
  if not size or not align or align&(align-1):status=-22;nextused=used
  else:
   start=((3+used+align-1)//align)*align-3;nextused=start+size;status=-12 if nextused>2045 else 0
   if status:nextused=used
  lines+=['    store(Int64(42), out)',f'    Int32(rc) = call luxrt_arena_alloc(arena, Int64({size}), Int64({align}), out)',f'    Int32(same) = eq(rc, {status})','    Int32(ok) = and(ok, same)','    Ptr(value) = load(out)']
  if status:lines+=['    Int32(same) = eq(value, 42)']
  else:lines+=[f'    Ptr(expected) = add(base, {start})','    Int32(same) = eq(value, expected)',f'    store(Int8({i%127}), value)','    Int32(byte) = load_u8(value)',f'    Int32(stored) = eq(byte, {i%127})','    Int32(ok) = and(ok, stored)']
  used=nextused;lines+=['    Int32(ok) = and(ok, same)','    Int64(used) = load(used_slot)',f'    Int32(same) = eq(used, {used})','    Int32(ok) = and(ok, same)']
  if i%16==15:
   lines+=['    Int32(rc) = call luxrt_arena_rewind(arena, Int64(0))','    Int32(same) = eq(rc, 0)','    Int32(ok) = and(ok, same)'];used=0
 lines+=['    Int32(status) = xor(ok, 1)','    return status','end'];return '\n'.join(lines)+'\n'
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=CONF,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();assemblies={};report={'target':a.target,'raw_allocation_cases':256,'runner':CONF[a.target]}
 with tempfile.TemporaryDirectory(prefix='luxrt-arena-') as d:
  d=Path(d);driver=ROOT/'tests/runtime/arena_driver.c';exe=d/'oracle';flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror']
  subprocess.run(flags+[str(driver),str(ROOT/'runtime/arena.c'),'-o',str(exe)],check=True);expected=subprocess.check_output([str(exe)],timeout=30);print(expected.decode().strip(),flush=True);report['oracle']=expected.decode().strip()
  source=d/'fixture.lir';source.write_text(fixture());previous=None
  for index,backend in enumerate(a.backend):
   for target in TARGETS:
    asm=subprocess.check_output([str(backend.resolve()),'-t',target,str(ROOT/'runtime/arena.lir')],timeout=30)
    if target in assemblies:assert asm==assemblies[target]
    assemblies[target]=asm
    if target=='amd64_sysv':
     p=d/'arena.s';p.write_bytes(asm);subprocess.run(flags+[str(driver),str(p),'-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],timeout=30)==expected
   binary=d/('raw'+str(index));subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(backend.resolve()),'-t',a.target,'-o',str(binary),str(source)],check=True,timeout=60)
   r=subprocess.run(CONF[a.target]+[str(binary)],capture_output=True,timeout=10);assert r.returncode==0 and not r.stdout and not r.stderr,r
   blob=binary.read_bytes();assert previous is None or previous==blob;previous=blob
  subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',str(driver),str(ROOT/'runtime/arena.c'),'-o',str(exe)],check=True);assert subprocess.check_output([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=30)==expected
  report.update(assembly_parity_targets=TARGETS,raw_binary_identical=True,c_asan_ubsan=True,leak_sanitizer=False,managed_gc=False)
  if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
  print(json.dumps(report))
if __name__=='__main__':main()
