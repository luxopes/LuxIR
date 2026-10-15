#!/usr/bin/env python3
"""Testy cíle c32 (C pro 32bitové architektury): tests/run/*.lir se přeloží
na C (luxir -t c32), clangem pro riscv32, arm (hard-float) a i686, slinkují
se samostatným runtime tests/c32/rt.c a runtime/c32/builtins.c (bez libc)
a spustí v qemu-user; výstup se porovná s tests/run/*.out.

Použití: run_c32.py --backend LUXIR [--arch riscv32|arm|i686]...
"""
import argparse, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT.parent / 'runtime' / 'c32' / 'builtins.c'
ARCHES = {
    'riscv32': (['--target=riscv32-unknown-linux-gnu', '-march=rv32imafdc', '-mabi=ilp32d', '-mno-relax'], ['qemu-riscv32']),
    'arm': (['--target=armv7a-unknown-linux-gnueabihf', '-mfloat-abi=hard', '-mfpu=vfpv3-d16', '-marm'], ['qemu-arm']),
    'i686': (['--target=i686-unknown-linux-gnu', '-msse2', '-mfpmath=sse'], ['qemu-i386']),
}
# Programy, které c32 záměrně nepodporuje (s důvodem).
UNSUPPORTED = {
    'memory': 'aggregate parameters passed by value (LSL does not use them)',
    'calls': 'variadic function definitions (LSL does not use them)',
    'tls': 'compiler TLS needs thread setup by a libc (the LSL runtime keeps its own thread block)',
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--backend', action='append', required=True)
    parser.add_argument('--arch', action='append', choices=sorted(ARCHES))
    args = parser.parse_args()
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for backend in args.backend:
            for arch in args.arch or sorted(ARCHES):
                flags, runner = ARCHES[arch]
                common = ['clang'] + flags + ['-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-static', '-fuse-ld=lld', '-w']
                for source in sorted((ROOT / 'run').glob('*.lir')):
                    name = f'{arch}-{source.stem}'
                    if source.stem in UNSUPPORTED:
                        print(f'skip    {name}: {UNSUPPORTED[source.stem]}')
                        continue
                    c = tmp / f'{name}.c'
                    exe = tmp / name
                    r = subprocess.run([backend, '-t', 'c32', '-o', str(c), str(source)], capture_output=True, text=True)
                    if r.returncode:
                        print(f'FAIL    {name}: luxir: {r.stderr.strip()[:500]}')
                        failed += 1
                        continue
                    r = subprocess.run(common + ['-o', str(exe), str(c), str(ROOT / 'c32' / 'rt.c'), str(RUNTIME)], capture_output=True, text=True)
                    if r.returncode:
                        print(f'FAIL    {name}: clang: {r.stderr.strip()[:1500]}')
                        failed += 1
                        continue
                    r = subprocess.run(runner + [str(exe)], capture_output=True, timeout=60)
                    expected = source.with_suffix('.out').read_bytes()
                    if r.returncode or r.stdout != expected:
                        print(f'FAIL    {name}: status {r.returncode}')
                        print('  got     ', r.stdout[:400])
                        print('  expected', expected[:400])
                        failed += 1
                    else:
                        print(f'ok      {name}')
    print(f'c32: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
