#!/usr/bin/env python3
"""Automatic typed Value roots survive forced collection, Phi and error returns."""
import argparse,json,os,re,resource,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
TARGETS=['amd64_sysv','arm64','rv64']
ERRORS=[
 ('missing','runtime_abi "1"\nfunction f(Ptr(h), Value(v)) -> Value:\n return v\nend\n','Value locals require'),
 ('duplicate','runtime_abi "1"\nfunction f(Ptr(h)):\n gc_heap(h)\n gc_heap(h)\n return\nend\n','duplicate gc_heap'),
 ('late','runtime_abi "1"\nfunction f(Ptr(h)):\n Int64(x) = 1\n gc_heap(h)\n return\nend\n','precede body instructions'),
 ('wrongblock','runtime_abi "1"\nfunction f(Ptr(h)):\n goto other\nother:\n gc_heap(h)\n return\nend\n','entry block'),
 ('none','function f(Ptr(h)):\n gc_heap(h)\n return\nend\n','requires runtime ABI 1'),
 ('literal','runtime_abi "1"\nfunction f():\n gc_heap(Ptr(0))\n return\nend\n','heap parameter or symbol'),
 ('temporary','runtime_abi "1"\nfunction f():\n gc_heap(h)\n Ptr(h) = 0\n return\nend\n','heap parameter or symbol'),
]
ENTRY='''runtime_abi "1"
export function main() -> Int32:
    Ptr(count) = alloc8(8)
    Int32(status) = call luxrt_gcroot_cases(count)
    Int32(failed) = ne(status, 0)
    return failed
end
'''
BAD='''runtime_abi "1"
export function main() -> Int32:
    Ptr(h) = alloc8(336)
    Int32(status) = invoke luxrt_heap_init(h, Int64(1048576), Int64(4096)) else failed goto run
run:
    Int32(status) = call gc_unbalanced(h, Int64(1))
    return 0
failed:
    return 1
end
'''
def disable_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--target',choices=TARGETS,default='amd64_sysv');ap.add_argument('--report',type=Path);a=ap.parse_args();parity={};rows=[]
 with tempfile.TemporaryDirectory(prefix='luxir-gcroot-') as d:
  d=Path(d);common=[ROOT/('tests/runtime/'+x) for x in ('gcroot_cases.c','gcroot_main.c','managed_os_oracle.c')];rt=[ROOT/('runtime/'+x+'.c') for x in ('managed','memory','abi','hash')];flags=['cc','-O2','-Wall','-Wextra','-Werror'];expected=None
  for b in a.backend:
   for enabled in (False,True):
    asm=subprocess.check_output([str(b.resolve())]+(['-i'] if enabled else [])+[str(ROOT/'tests/gcroot.lir')]);p=d/'gcroot.s';p.write_bytes(asm)
    leaf=asm.split(b'gc_leaf:')[1].split(b'/* end function gc_leaf */')[0]
    assert b'luxrt_roots_' not in leaf
    exe=d/'host';subprocess.run(flags+list(map(str,common+rt+[p]))+['-o',str(exe)],check=True)
    output=subprocess.check_output([str(exe)],timeout=30)
    if expected is not None:assert expected==output
    expected=output;rows.append({'backend':str(b),'inline':enabled,'oracle':output.decode().strip()})
    if enabled:
     subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined']+list(map(str,common+rt+[p]))+['-o',str(exe)],check=True)
     assert subprocess.check_output([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=30)==expected
   for t in TARGETS:
    asm=subprocess.check_output([str(b.resolve()),'-t',t,str(ROOT/'tests/gcroot.lir')]);key=t
    if key in parity:assert parity[key]==asm
    parity[key]=asm
   for name,body,msg in ERRORS:
    source=d/'invalid.lir';source.write_text(body);r=subprocess.run([str(b.resolve()),str(source)],capture_output=True)
    assert r.returncode and msg.encode() in r.stderr,(name,r.stderr)
   # Value remains an ordinary aggregate type name in legacy external-runtime mode.
   source=d/'legacy.lir';source.write_text('type Value = struct(Int64)\nfunction f(Value(v)):\n return\nend\n')
   assert subprocess.run([str(b.resolve()),str(source)],capture_output=True).returncode==0
   source=d/'main.lir';source.write_text(ENTRY);exe=d/'raw';build=['python3',str(ROOT/'tools/luxir_build.py'),'--backend',str(b.resolve()),'-t',a.target,'-o',str(exe)]
   subprocess.run(build+[str(source),str(ROOT/'tests/gcroot.lir'),str(ROOT/'tests/runtime/gcroot_cases.lir')],check=True,timeout=90)
   runner={'amd64_sysv':[],'arm64':['qemu-aarch64'],'rv64':['qemu-riscv64']}[a.target]
   assert subprocess.run(runner+[str(exe)],timeout=30).returncode==0
   # An unbalanced manual frame must trap before returning dangling stack roots.
   source.write_text(BAD);subprocess.run(build+[str(source),str(ROOT/'tests/gcroot.lir'),str(ROOT/'tests/runtime/gcroot_cases.lir')],check=True,timeout=90)
   r=subprocess.run(runner+[str(exe)],capture_output=True,timeout=30,preexec_fn=disable_core)
   assert r.returncode not in (0,1),r
 report={'target':a.target,'results':rows,'assembly_parity_targets':TARGETS,'raw_forced_gc_execution':True,'unbalanced_manual_frame_traps':True,'c_runtime_asan_ubsan':True,'syntax_rejections':len(ERRORS),'legacy_Value_type_preserved':True,'models':['unrooted input parameter','automatic allocation locals','Phi root with cleared previous owner','borrowed payload owner','invoke error returns','heap parameter reassignment','leaf root elision','all frames popped and mappings released'],'mode':'single-owner precise shadow roots, whole-function local lifetime; no native PC stackmaps or STW'}
 if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
