#!/usr/bin/env python3
"""Managed numeric adapters and immutable tuples: oracle, GC and raw Linux."""
import argparse, json, os, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
TARGETS = ['amd64_sysv', 'amd64_win', 'amd64_apple', 'arm64', 'arm64_apple', 'rv64']
RUNNERS = {'amd64_sysv': [], 'arm64': ['qemu-aarch64'], 'rv64': ['qemu-riscv64']}
MODULES = ['memory', 'integer', 'float64', 'managed', 'collections', 'numeric', 'tuple']
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', type=Path, action='append', required=True)
    ap.add_argument('--target', choices=RUNNERS, default='amd64_sysv')
    ap.add_argument('--inline', action='store_true')
    ap.add_argument('--report', type=Path)
    a = ap.parse_args()
    common = [ROOT / ('tests/runtime/' + f) for f in ['extended_cases.c','extended_main.c','managed_os_oracle.c']]
    reference = [ROOT / ('runtime/' + m + '.c') for m in MODULES]
    flags = ['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'runtime/vendor')]
    parity = {}
    with tempfile.TemporaryDirectory(prefix='luxrt-extended-') as d:
        d = Path(d); exe = d/'oracle'
        subprocess.run(flags+list(map(str,common+reference))+['-o',str(exe)],check=True)
        expected = subprocess.check_output([str(exe)],timeout=60)
        print(expected.decode().strip(),flush=True)
        previous = None
        for b in a.backend:
            host = []
            for m in MODULES:
                for t in TARGETS:
                    asm = subprocess.check_output([str(b.resolve())]+(['-i'] if a.inline else [])+['-t',t,str(ROOT/('runtime/'+m+'.lir'))],timeout=60)
                    key = m,t
                    if key in parity: assert parity[key] == asm, key
                    parity[key] = asm
                    if t == 'amd64_sysv':
                        p = d/(m+'.s');p.write_bytes(asm);host.append(p)
            subprocess.run(flags+list(map(str,common+host))+['-o',str(exe)],check=True)
            assert subprocess.check_output([str(exe)],timeout=60) == expected
            binary = d/'raw'
            subprocess.run(['python3',str(ROOT/'tools/luxir_build.py'),'--managed']+(['--inline'] if a.inline else [])+['--backend',str(b.resolve()),'-t',a.target,'-o',str(binary),str(ROOT/'tests/runtime/extended_entry.lir'),str(ROOT/'tests/runtime/extended_cases.lir')],check=True,timeout=90)
            r = subprocess.run(RUNNERS[a.target]+[str(binary)],capture_output=True,timeout=60)
            assert r.returncode == 0 and not r.stdout and not r.stderr,r
            blob = binary.read_bytes()
            assert previous is None or previous == blob
            previous = blob
        subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined','-I'+str(ROOT/'runtime/vendor')]+list(map(str,common+reference))+['-o',str(exe)],check=True)
        assert subprocess.check_output([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),timeout=60) == expected
    report = {'target':a.target,'inline':a.inline,'oracle':expected.decode().strip(),'assembly_parity_targets':TARGETS,'raw_binary_identical':True,'c_asan_ubsan':True,'all_mappings_released':True,'models':['full Int64 managed conversion','Float64 bit roundtrip and raw text comparison','immutable Tuple list model','forced GC during construction','slice/concat/source mutation','foreign heap and OOM preservation'],'runner':RUNNERS[a.target]}
    if a.report: a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))
if __name__ == '__main__': main()
