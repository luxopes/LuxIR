#!/usr/bin/env python3
"""Local-binding streaming inliner: independent scalar oracle and exclusions."""
import argparse, ctypes, json, random, re, subprocess, tempfile
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from qbe2lir import convert
SSA = '''
export function l $mix(l %x, l %y) {
@start
 %a =l xor %x, %y
 %b =l mul %a, 6364136223846793005
 %c =l shr %x, 17
 %d =l add %b, %c
 ret %d
}
export function l $twice(l %x, l %y) {
@start
 %x =l call $mix(l %x, l %y)
 %x =l call extern $mix(l %y, l %x)
 ret %x
}
export function d $fp(d %x) {
@start
 %a =d mul %x, d_1.5
 %b =d add %a, d_0.25
 ret %b
}
export function d $fpcaller(d %x) {
@start
 %y =d call $fp(d %x)
 ret %y
}
export function l $read(l %p) {
@start
 %x =l loadl %p
 ret %x
}
export function l $readcaller(l %p, l %v) {
@start
 %a =l call $read(l %p)
 storel %v, %p
 %b =l call $read(l %p)
 %c =l xor %a, %b
 ret %c
}
export function l $write(l %p, l %v) {
@start
 storel %v, %p
 ret %v
}
export function l $writecaller(l %p, l %v) {
@start
 %r =l call $write(l %p, l %v)
 ret %r
}
export function l $forwardcaller(l %x) {
@start
 %y =l call $later(l %x)
 ret %y
}
export function l $later(l %x) {
@start
 %a =l add %x, 1
 ret %a
}
export function l $indirect(l %f, l %x, l %y) {
@start
 %r =l call %f(l %x, l %y)
 ret %r
}
'''
# Side effects, ABI-sensitive signatures and recursive control flow must stay calls.
EXCLUSIONS = r'''function side(Ptr(p), Int64(x)) -> Int64:
    Int64(y) = atomic_add(p, x)
    return y
end
function stack(Int64(x)) -> Int64:
    Ptr(p) = alloc8(16)
    store(x, p)
    return p
end
function variadic(Int64(x), ...) -> Int64:
    return x
end
function environment(Env(e), Int64(x)) -> Int64:
    return x
end
function recursive(Int64(x)) -> Int64:
    Int64(y) = call recursive(x)
    return y
end
export function excluded(Ptr(p), Int64(x)) -> Int64:
    Int64(a) = call side(p, x)
    Int64(b) = call stack(x)
    Int64(c) = call variadic(x, ..., x)
    Int64(d) = call environment(Env(p), x)
    Int64(e) = call recursive(x)
    Int64(s) = add(a, b)
    Int64(s) = add(s, c)
    Int64(s) = add(s, d)
    Int64(s) = add(s, e)
    return s
end
'''
MASK = (1 << 64) - 1
def oracle(x,y): return (((x^y)*6364136223846793005)+(x>>17)) & MASK

def verify(lib):
 u=ctypes.c_uint64; p=ctypes.POINTER(u)
 lib.twice.argtypes=[u,u];lib.twice.restype=u
 lib.fpcaller.argtypes=[ctypes.c_double];lib.fpcaller.restype=ctypes.c_double
 lib.readcaller.argtypes=[p,u];lib.readcaller.restype=u
 lib.writecaller.argtypes=[p,u];lib.writecaller.restype=u
 lib.forwardcaller.argtypes=[u];lib.forwardcaller.restype=u
 lib.indirect.argtypes=[ctypes.c_void_p,u,u];lib.indirect.restype=u
 rng=random.Random(0x1A11);checks=0
 for _ in range(20000):
  x=rng.getrandbits(64);y=rng.getrandbits(64)
  assert lib.twice(x,y)==oracle(y,oracle(x,y));checks+=1
  v=u(x);assert lib.readcaller(ctypes.byref(v),y)==x^y and v.value==y;checks+=1
  assert lib.writecaller(ctypes.byref(v),x)==x and v.value==x;checks+=1
  assert lib.forwardcaller(x)==(x+1)&MASK;checks+=1
  assert lib.indirect(ctypes.cast(lib.mix,ctypes.c_void_p),x,y)==oracle(x,y);checks+=1
  f=rng.uniform(-1e100,1e100);assert lib.fpcaller(f)==f*1.5+0.25;checks+=1
 return checks

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);a=ap.parse_args();rows=[];parity={}
 with tempfile.TemporaryDirectory(prefix='luxir-inline-') as tmp:
  tmp=Path(tmp);lir=tmp/'test.lir';lir.write_text(convert(SSA))
  for b in a.backend:
   for enabled in (False,True):
    asm=subprocess.check_output([str(b.resolve())]+(['-i'] if enabled else [])+[str(lir)])
    # Ineligible stores, forward and indirect calls survive; pure calls disappear.
    if enabled:
     assert not re.search(rb'callq? (mix|fp|read)(?:@PLT)?\n',asm)
     assert re.search(rb'callq? write',asm) and re.search(rb'callq? later',asm) and re.search(rb'callq? \*',asm)
    s=tmp/'test.s';s.write_bytes(asm);so=tmp/('test'+str(len(rows))+'.so')
    subprocess.run(['cc','-shared','-fPIC','-Wl,-Bsymbolic',str(s),'-o',str(so)],check=True)
    rows.append({'backend':str(b),'inline':enabled,'checks':verify(ctypes.CDLL(str(so)))})
   extra=tmp/'excluded.lir';extra.write_text(EXCLUSIONS)
   excluded=subprocess.check_output([str(b.resolve()),'-i',str(extra)])
   for name in (b'side',b'stack',b'variadic',b'environment',b'recursive'):
    assert re.search(rb'callq? '+name+rb'(?:@PLT)?\n',excluded),name
   # Cache must reset between separate input files, despite interned symbol IDs.
   first=tmp/'first.lir';second=tmp/'second.lir'
   first.write_text(convert(SSA.split('export function l $twice')[0]))
   second.write_text('function separate(Int64(x), Int64(y)) -> Int64:\n Int64(z) = call mix(x, y)\n return z\nend\n')
   separated=subprocess.check_output([str(b.resolve()),'-i',str(first),str(second)])
   assert re.search(rb'callq? mix',separated)
   # Bounded expansion and cache growth must retain fallback calls.
   bounded=tmp/'bounded.lir'
   many='\n'.join(f' Int64(x) = call mix(x, Int64({i}))' for i in range(300))
   bounded.write_text(first.read_text()+'function many(Int64(x)) -> Int64:\n'+many+'\n return x\nend\n')
   boundedasm=subprocess.check_output([str(b.resolve()),'-i',str(bounded)])
   assert re.search(rb'callq? mix',boundedasm)
   cachetext=''.join(f'function v{i}(Int64(x)) -> Int64:\n Int64(y) = add(x, {i+1})\n return y\nend\n' for i in range(140))
   cachetext+='function cachelimit(Int64(x)) -> Int64:\n Int64(a) = call v0(x)\n Int64(b) = call v139(a)\n return b\nend\n'
   bounded.write_text(cachetext)
   boundedasm=subprocess.check_output([str(b.resolve()),'-i',str(bounded)])
   assert not re.search(rb'callq? v0\n',boundedasm) and re.search(rb'callq? v139\n',boundedasm)
   for t in ['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']:
    asm=subprocess.check_output([str(b.resolve()),'-i','-t',t,str(lir)])
    if t in parity:assert parity[t]==asm,t
    parity[t]=asm
    source=tmp/'target.s';source.write_bytes(asm)
    triple={'amd64_sysv':'x86_64-linux-gnu','amd64_win':'x86_64-w64-windows-gnu','amd64_apple':'x86_64-apple-macos13','arm64':'aarch64-linux-gnu','arm64_apple':'arm64-apple-macos13','rv64':'riscv64-linux-gnu'}[t]
    subprocess.run(['clang','--target='+triple]+(['-march=rv64gc'] if t=='rv64' else [])+['-c',str(source),'-o',str(tmp/'target.o')],check=True)
 report={'results':rows,'assembly_parity_targets':list(parity),'assembled_targets':list(parity),'fallback_checks':['store','atomic','stack allocation','varargs','environment','recursive','forward','indirect','separate inputs','expansion limit','cache limit'],'oracle':'independent unsigned arithmetic and host scalar double','limitations':'already-defined bounded single-block scalar leaves only; -i promises local binding'}
 if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
