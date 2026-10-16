#!/usr/bin/env python3
"""Testy 32bitových cílů s assemblerem (i386, arm): tests/run/*.lir se
přeloží backendem LuxIR (-t i386 / -t arm) spolu s pomocnými funkcemi
runtime/w32/helpers.lir, sestaví GNU as, slinkují se samostatným runtime
testů tests/c32/rt.c (přeložený clangem jen pro testy, s konvencí c32:
ukazatele jako 64bitová slova) a spustí (i386 nativně, arm v qemu-arm).
Výstup se porovná s tests/run/*.out.

Použití: run_w32.py --backend LUXIR [--arch i386|arm]...
"""
import argparse, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
HELPERS = ROOT.parent / 'runtime' / 'w32' / 'helpers.lir'
BUILTINS = ROOT.parent / 'runtime' / 'c32' / 'builtins.c'
ARCHES = {
    'i386': (['as', '--32'], ['--target=i686-unknown-linux-gnu', '-msse2', '-mfpmath=sse'], ['-m', 'elf_i386'], []),
    'arm': (['arm-linux-gnueabihf-as', '-march=armv7-a', '-mfpu=vfpv3-d16', '-mfloat-abi=hard'],
            ['--target=armv7a-unknown-linux-gnueabihf', '-mfloat-abi=hard', '-mfpu=vfpv3-d16', '-marm'],
            ['-m', 'armelf_linux_eabi'], ['qemu-arm']),
}
UNSUPPORTED = {
    'memory': 'aggregate parameters passed by value (LSL does not use them)',
    'calls': 'variadic function definitions (LSL does not use them)',
    'tls': 'compiler TLS needs thread setup by a libc (the LSL runtime keeps its own thread block)',
}


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, **kw)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--backend', action='append', required=True)
    parser.add_argument('--arch', action='append', choices=sorted(ARCHES))
    parser.add_argument('--test', action='append', help='jen vybrané testy (jméno bez .lir)')
    args = parser.parse_args()
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for backend in args.backend:
            for arch in args.arch or sorted(ARCHES):
                asm, cflags, ldflags, runner = ARCHES[arch]
                common = ['clang'] + cflags + ['-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-w', '-c']
                objs = []
                for name, source in [('rt', ROOT / 'c32' / 'rt.c'), ('builtins', BUILTINS)]:
                    obj = tmp / f'{arch}-{name}.o'
                    r = run(common + ['-o', obj, source], text=True)
                    if r.returncode:
                        print(f'FAIL    {arch}: clang {name}: {r.stderr[:800]}')
                        return 1
                    objs.append(obj)
                hs = tmp / f'{arch}-helpers.s'
                ho = tmp / f'{arch}-helpers.o'
                r = run([backend, '-t', arch, '-o', hs, HELPERS], text=True)
                if r.returncode == 0:
                    r = run(asm + ['-o', ho, hs], text=True)
                if r.returncode:
                    print(f'FAIL    {arch}: helpers: {r.stderr.strip()[:1500]}')
                    failed += 1
                    continue
                objs.append(ho)
                for source in sorted((ROOT / 'run').glob('*.lir')):
                    if args.test and source.stem not in args.test:
                        continue
                    name = f'{arch}-{source.stem}'
                    if source.stem in UNSUPPORTED:
                        print(f'skip    {name}: {UNSUPPORTED[source.stem]}')
                        continue
                    s = tmp / f'{name}.s'
                    o = tmp / f'{name}.o'
                    exe = tmp / name
                    r = run([backend, '-t', arch, '-o', s, source], text=True)
                    if r.returncode:
                        print(f'FAIL    {name}: luxir: {r.stderr.strip()[:500]}')
                        failed += 1
                        continue
                    r = run(asm + ['-o', o, s], text=True)
                    if r.returncode:
                        print(f'FAIL    {name}: as: {r.stderr.strip()[:1500]}')
                        failed += 1
                        continue
                    r = run(['ld.lld'] + ldflags + ['-static', '-o', exe, o] + objs, text=True)
                    if r.returncode:
                        print(f'FAIL    {name}: ld: {r.stderr.strip()[:1500]}')
                        failed += 1
                        continue
                    r = run(runner + [exe], timeout=60)
                    expected = source.with_suffix('.out').read_bytes()
                    if r.returncode or r.stdout != expected:
                        print(f'FAIL    {name}: status {r.returncode}')
                        print('  got     ', r.stdout[:600])
                        print('  expected', expected[:600])
                        failed += 1
                    else:
                        print(f'ok      {name}')
    print(f'w32: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
