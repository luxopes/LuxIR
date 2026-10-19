#!/usr/bin/env python3
"""Vestavěný linker (luxir -l): tests/run/*.lir se přeloží backendem,
sestaví vestavěným assemblerem a slinkují s testovacím runtime
tests/c32/rt.c (clang jen pro testy, bez libc) pro x86-64 (statický ELF
bez PIE i PIE) a i386; programy se spustí a výstup porovná s .out. Navíc se
kontroluje smlouva obrazu: bez interpretu, bez RWX segmentu, GNU_STACK bez
práva spouštění, bez tabulky symbolů.

Použití: ld_check.py --backend LUXIR [--backend …] [--arch x86_64|i386|arm64|rv64]
"""
import argparse, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILTINS = ROOT.parent / 'runtime' / 'c32' / 'builtins.c'
ARCH = {
    # PIE: testovací runtime jako -fPIE (odkazy relativně k PC), bez PIE -fno-pic
    'x86_64': ('amd64_sysv', ['--target=x86_64-linux-gnu', '-mno-red-zone'], [], [['-fno-pic'], ['-fPIE', '-p']]),
    'i386': ('i386', ['--target=i686-linux-gnu', '-msse2', '-mfpmath=sse'], [BUILTINS], [['-fno-pic']]),
    'arm64': ('arm64', ['--target=aarch64-linux-gnu'], [], [['-fPIE', '-p']]),
    'rv64': ('rv64', ['--target=riscv64-linux-gnu', '-march=rv64g', '-mabi=lp64d', '-mno-relax'], [], [['-fno-pic'], ['-fPIE', '-p']]),
}
RUNNER = {'arm64': ['qemu-aarch64'], 'rv64': ['qemu-riscv64']}
UNSUPPORTED = {'memory', 'calls', 'tls'}   # i386: agregáty a variadické; tls: TLS nepodporuje linker


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, **kw)


def contract(path, pie):
    d = path.read_bytes()
    assert d[:4] == b'\x7fELF'
    elf64 = d[4] == 2
    if elf64:
        e_type, machine = struct.unpack_from('<HH', d, 16)
        phoff, = struct.unpack_from('<Q', d, 32)
        shoff, = struct.unpack_from('<Q', d, 40)
        phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', d, 54)
    else:
        e_type, machine = struct.unpack_from('<HH', d, 16)
        phoff, shoff = struct.unpack_from('<II', d, 28)
        phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', d, 42)
    assert e_type == (3 if pie else 2), 'ET_DYN for PIE, ET_EXEC otherwise'
    stacks = 0
    for i in range(phnum):
        p = d[phoff + i * phentsize:]
        if elf64:
            ptype, flags = struct.unpack_from('<II', p)
        else:
            ptype, = struct.unpack_from('<I', p)
            flags, = struct.unpack_from('<I', p, 24)
        assert ptype != 3, 'PT_INTERP'
        if ptype == 1:
            assert flags & 3 != 3, 'RWX segment'
        if ptype == 0x6474e551:
            stacks += 1
            assert flags & 1 == 0, 'executable stack'
    assert stacks == 1, 'GNU_STACK'
    for i in range(shnum):
        sh_type, = struct.unpack_from('<I', d, shoff + i * shentsize + 4)
        assert sh_type != 2, 'symbol table present'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', action='append', required=True)
    ap.add_argument('--arch', action='append', choices=sorted(ARCH))
    a = ap.parse_args()
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for arch in a.arch or sorted(ARCH):
            target, cflags, extra, modes = ARCH[arch]
            for backend in a.backend:
                backend = str(Path(backend).resolve())
                helpers = []
                if arch == 'i386':
                    # pomocné funkce 64bitového dělení a převodů (w32)
                    hs, ho = tmp / 'helpers.s', tmp / 'helpers.o'
                    r = run([backend, '-t', target, '-o', hs, ROOT.parent / 'runtime' / 'w32' / 'helpers.lir'], text=True)
                    if r.returncode == 0:
                        r = run([backend, '-a', '-t', target, '-o', ho, hs], text=True)
                    if r.returncode:
                        print(f'FAIL    {arch} helpers: {r.stderr.strip()[:300]}')
                        failed += 1
                        continue
                    helpers = [ho]
                for lir in sorted((ROOT / 'run').glob('*.lir')):
                    if lir.stem == 'tls' or (lir.stem in UNSUPPORTED and arch == 'i386'):
                        continue
                    for mode in modes:
                        pie = '-p' in mode
                        name = f'{arch}{"-pie" if pie else ""}-{lir.stem}'
                        objs = []
                        for src in [ROOT / 'c32' / 'rt.c'] + extra:
                            obj = tmp / f'{arch}-{int(pie)}-{src.stem}.o'
                            if not obj.exists():
                                r = run(['clang'] + cflags + [f for f in mode if f != '-p'] + ['-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-w', '-c', '-o', obj, src], text=True)
                                if r.returncode:
                                    print(r.stderr[:800])
                                    return 1
                            objs.append(obj)
                        s, o, exe = tmp / 'p.s', tmp / 'p.o', tmp / 'p'
                        # rv64: PIE potřebuje i kód s adresami relativně k PC (-p backendu)
                        r = run([backend, '-t', target] + (['-p'] if pie and arch == 'rv64' else []) + ['-o', s, lir], text=True)
                        if r.returncode == 0:
                            r = run([backend, '-a', '-t', target, '-o', o, s], text=True)
                        if r.returncode == 0:
                            r = run([backend, '-l'] + (['-p'] if pie else []) + ['-t', target, '-o', exe, o] + helpers + objs, text=True)
                        if r.returncode and pie and 'absolute relocation' in r.stderr and str(o) in r.stderr:
                            # program s absolutními adresami v datech nemůže být PIE bez zavaděče
                            print(f'skip    {name}: absolute addresses in data')
                            continue
                        if r.returncode:
                            print(f'FAIL    {name}: {r.stderr.strip()[:400]}')
                            failed += 1
                            continue
                        try:
                            contract(exe, pie)
                        except AssertionError as e:
                            print(f'FAIL    {name}: image contract: {e}')
                            failed += 1
                            continue
                        r = run(RUNNER.get(arch, []) + [exe], timeout=60)
                        expected = lir.with_suffix('.out').read_bytes()
                        if r.returncode or r.stdout != expected:
                            print(f'FAIL    {name}: status {r.returncode}')
                            print('  got     ', r.stdout[:300])
                            print('  expected', expected[:300])
                            failed += 1
                        else:
                            print(f'ok      {name}')
    print(f'ld_check: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
