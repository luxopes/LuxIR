#!/usr/bin/env python3
"""SipHash official vectors, independent Python oracle and seeded heap models."""
import argparse, ctypes, json, os, random, struct, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
RUNNERS={'amd64_sysv':[],'arm64':['qemu-aarch64'],'rv64':['qemu-riscv64']}
MODULES=['memory','hash','managed','collections','map','io','secure']
MASK=(1<<64)-1
def oracle(data,k0,k1):
    v=[0x736f6d6570736575^k0,0x646f72616e646f6d^k1,0x6c7967656e657261^k0,0x7465646279746573^k1]
    def rot(x,n):return ((x<<n)|(x>>(64-n)))&MASK
    def rounds(count):
        for _ in range(count):
            for a,b,n in [(0,1,13),(2,3,16),(0,3,21),(2,1,17)]:
                v[a]=(v[a]+v[b])&MASK;v[b]=rot(v[b],n)^v[a]
                if (a,b)==(0,1):v[0]=rot(v[0],32)
                if (a,b)==(2,1):v[2]=rot(v[2],32)
    at=0
    while len(data)-at>=8:
        word=int.from_bytes(data[at:at+8],'little');v[3]^=word;rounds(2);v[0]^=word;at+=8
    word=(len(data)&255)<<56|int.from_bytes(data[at:],'little')
    v[3]^=word;rounds(2);v[0]^=word;v[2]^=255;rounds(4)
    return v[0]^v[1]^v[2]^v[3]
def run(command,**kw):return subprocess.run(list(map(str,command)),check=True,timeout=90,**kw)
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend',type=Path,action='append',required=True)
    ap.add_argument('--target',choices=RUNNERS,default='amd64_sysv');ap.add_argument('--report',type=Path)
    a=ap.parse_args();parity={};rng=random.Random(191);samples=[]
    for i in range(3000):
        data=rng.randbytes(i%257);k0=rng.getrandbits(64);k1=rng.getrandbits(64)
        samples.append((data,k0,k1,oracle(data,k0,k1)))
    for size in [512,1024,4096,8192,100000]:
        data=rng.randbytes(size);k0=rng.getrandbits(64);k1=rng.getrandbits(64);samples.append((data,k0,k1,oracle(data,k0,k1)))
    common=[ROOT/('tests/runtime/'+f) for f in ['hash_cases.c','hash_main.c']]
    native=ROOT/'runtime/linux-x86_64';osfiles=[native/'os.c',native/'syscall.S']
    refs=[ROOT/('runtime/'+m+'.c') for m in MODULES]
    flags=['cc','-O2','-Wall','-Wextra','-Werror']
    with tempfile.TemporaryDirectory(prefix='luxrt-hash-') as directory:
        d=Path(directory);exe=d/'test';run(flags+common+refs+osfiles+['-o',exe])
        expected=subprocess.check_output([str(exe),'unused'],timeout=60);print(expected.decode().strip(),flush=True)
        libraries=[]
        def raw_hash(source):
            lib=d/('hash'+str(len(libraries))+'.so');run(['cc','-shared','-fPIC','-O2',source,'-o',lib])
            library=ctypes.CDLL(str(lib));libraries.append(library);f=library.luxrt_siphash24
            f.argtypes=[ctypes.c_void_p,ctypes.c_uint64,ctypes.c_uint64,ctypes.c_uint64];f.restype=ctypes.c_uint64
            for i,(data,k0,k1,want) in enumerate(samples):
                offset=i%8;buffer=ctypes.create_string_buffer(b'X'*offset+data)
                assert f(ctypes.byref(buffer,offset),len(data),k0,k1)==want,i
        raw_hash(ROOT/'runtime/hash.c')
        renamed=d/'os.o'
        run(flags+['-Dluxrt_os_random=test_real_random','-c',native/'os.c','-o',renamed])
        entropy_common=[ROOT/'tests/runtime/hash_entropy.c',renamed,native/'syscall.S']
        run(flags+entropy_common+refs+['-o',exe])
        entropy_expected=subprocess.check_output([str(exe)],timeout=30)
        previous=None
        entry=d/'entry.lir';entry.write_text('ir_version "1"\nruntime_abi "1"\nexport function main() -> Int32:\n Ptr(count) = alloc8(8)\n Int32(status) = call luxrt_hash_cases(Ptr(0),count)\n return status\nend\n')
        for backend in a.backend:
            backend=backend.resolve();host=[]
            for module in MODULES:
                for target in TARGETS:
                    asm=subprocess.check_output([str(backend),'-t',target,str(ROOT/('runtime/'+module+'.lir'))],timeout=60)
                    key=module,target
                    if key in parity:assert parity[key]==asm,key
                    parity[key]=asm
                    if target=='amd64_sysv':
                        p=d/(module+'.s');p.write_bytes(asm);host.append(p)
                        if module=='hash':raw_hash(p)
            run(flags+common+host+osfiles+['-o',exe]);assert subprocess.check_output([str(exe),'unused'],timeout=60)==expected
            run(flags+entropy_common+host+['-o',exe]);assert subprocess.check_output([str(exe)],timeout=30)==entropy_expected
            binary=d/'raw';run(['python3',ROOT/'tools/luxir_build.py','--backend',backend,'-t',a.target,'-o',binary,entry,ROOT/'tests/runtime/hash_cases.lir'])
            r=run(RUNNERS[a.target]+[binary],capture_output=True);assert not r.stdout and not r.stderr
            blob=binary.read_bytes();assert previous is None or previous==blob;previous=blob
        run(['cc','-O1','-g','-fsanitize=address,undefined']+common+refs+osfiles+['-o',exe])
        assert subprocess.check_output([str(exe),'unused'],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=60)==expected
        run(['cc','-O1','-g','-fsanitize=address,undefined']+entropy_common+refs+['-o',exe])
        assert subprocess.check_output([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=60)==entropy_expected
    report={'target':a.target,'oracle':expected.decode().strip(),'independent_python_vectors':len(samples),'official_vectors':64,'entropy_faults':entropy_expected.decode().strip(),'assembly_parity_targets':TARGETS,'raw_binary_identical':True,'c_asan_ubsan':True,'models':['keyed cached strings','foreign-heap read-only map lookup/deletion','same-heap insertion ownership','numeric key equivalence','seed propagation across slabs','reseed rejection and empty-heap reseed','zero key','OOM preservation'],'runner':RUNNERS[a.target]}
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))
if __name__=='__main__':main()
