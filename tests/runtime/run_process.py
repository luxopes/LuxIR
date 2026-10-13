#!/usr/bin/env python3
"""Raw spawn/wait: descriptor, signal, error handoff and cleanup contracts."""
import argparse, json, os, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
TARGETS = ['amd64_sysv', 'arm64', 'rv64']
RUNNERS = {'amd64_sysv': [], 'arm64': ['qemu-aarch64'], 'rv64': ['qemu-riscv64']}
MODULES = ['process', 'io', 'memory']
def run(command, **kwargs):
    return subprocess.run(list(map(str,command)),check=True,timeout=90,**kwargs)
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend',type=Path,action='append',required=True)
    ap.add_argument('--target',choices=TARGETS,default='amd64_sysv')
    ap.add_argument('--report',type=Path)
    ap.add_argument('--capture',action='store_true',help='run bounded concurrent capture corpus')
    a=ap.parse_args();parity={}
    modules=MODULES+(['capture'] if a.capture else [])
    corpus='capture' if a.capture else 'process'
    flags=['cc','-O2','-std=c11','-Wall','-Wextra','-Werror']
    common=[ROOT/('tests/runtime/'+corpus+'_cases.c'),ROOT/('tests/runtime/'+corpus+'_main.c')]
    native=ROOT/'runtime/linux-x86_64'
    osfiles=[native/n for n in ['os.c','signal.c','syscall.S','signal.S']]
    refs=[ROOT/('runtime/'+m+'.c') for m in modules]
    with tempfile.TemporaryDirectory(prefix='luxrt-process-') as directory:
        d=Path(directory);child=d/'child';exe=d/'test'
        run(flags+[ROOT/'tests/runtime/process_child.c','-o',child])
        run(flags+common+refs+osfiles+['-o',exe])
        expected=subprocess.check_output([str(exe),str(child)],timeout=60)
        print(expected.decode().strip(),flush=True)
        # Force the old-kernel procfs fallback, and reject a substituted filesystem.
        shim=d/'fallback.c'
        shim.write_text('''#include <stdint.h>
int64_t test_real_close_range(uint64_t,uint64_t,uint64_t);
int64_t test_real_fstatfs(int64_t,void*);
int test_bad_proc;
int64_t luxrt_os_close_range(uint64_t a,uint64_t b,uint64_t c){(void)a;(void)b;(void)c;return -38;}
int64_t luxrt_os_fstatfs(int64_t f,void *p){int64_t s=test_real_fstatfs(f,p);if(!s&&test_bad_proc)((uint64_t*)p)[0]=0;return s;}
''')
        renamed=d/'os.o'
        run(flags+['-Dluxrt_os_close_range=test_real_close_range','-Dluxrt_os_fstatfs=test_real_fstatfs','-c',native/'os.c','-o',renamed])
        fallback_os=[renamed]+osfiles[1:]+[shim]
        run(flags+common+refs+fallback_os+['-o',exe])
        assert subprocess.check_output([str(exe),str(child)],timeout=60)==expected
        bad=d/'bad.c'
        bad.write_text('''#include <stdint.h>
#include "process.h"
extern int test_bad_proc;
int main(int argc,char **argv){(void)argc;test_bad_proc=1;const char *args[]={"child","exit",0};int64_t pid=999;return !(luxrt_process_spawn(argv[1],args,0,-1,-1,-1,&pid)==-5&&pid==999);}
''')
        run(flags+['-I'+str(ROOT/'runtime'),bad]+refs+fallback_os+['-o',exe])
        run([exe,child])
        entry=d/'entry.lir'
        path_literal=''.join('\\x%02x'%x for x in str(child).encode()+b'\0')
        entry.write_text('ir_version "1"\nruntime_abi "1"\ndata child_path align 1 = "'+path_literal+'"\nexport function main() -> Int32:\n Ptr(count) = alloc8(8)\n Int32(status) = call luxrt_'+corpus+'_cases(child_path,count)\n return status\nend\n')
        previous=None
        for backend in a.backend:
            backend=backend.resolve();host=[]
            for module in modules:
                for target in TARGETS:
                    asm=subprocess.check_output([str(backend),'-t',target,str(ROOT/('runtime/'+module+'.lir'))],timeout=60)
                    key=module,target
                    if key in parity:assert parity[key]==asm,key
                    parity[key]=asm
                    if target=='amd64_sysv':
                        p=d/(module+'.s');p.write_bytes(asm);host.append(p)
            run(flags+common+host+osfiles+['-o',exe])
            assert subprocess.check_output([str(exe),str(child)],timeout=60)==expected
            run(flags+common+host+fallback_os+['-o',exe])
            assert subprocess.check_output([str(exe),str(child)],timeout=60)==expected
            run(flags+['-I'+str(ROOT/'runtime'),bad]+host+fallback_os+['-o',exe]);run([exe,child])
            binary=d/'raw'
            run(['python3',ROOT/'tools/luxir_build.py','--backend',backend,'-t',a.target,'-o',binary,entry,ROOT/('tests/runtime/'+corpus+'_cases.lir')])
            result=subprocess.run(RUNNERS[a.target]+[str(binary)],capture_output=True,timeout=90)
            assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
            blob=binary.read_bytes();assert previous is None or previous==blob;previous=blob
        run(['cc','-O1','-g','-fsanitize=address,undefined']+common+refs+osfiles+['-o',exe])
        assert subprocess.check_output([str(exe),str(child)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=60)==expected
    report={'capture':a.capture,'target':a.target,'oracle':expected.decode().strip(),'assembly_parity_targets':TARGETS,'raw_binary_identical':True,'c_asan_ubsan':True,'procfs_fallback':True,'substituted_procfs_rejected':True,'runner':RUNNERS[a.target],'emulator':subprocess.check_output(RUNNERS[a.target]+['--version']).decode().splitlines()[0] if RUNNERS[a.target] else None}
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))
if __name__=='__main__':main()
