#!/usr/bin/env python3
"""Vestavěný linker v režimu bare metal (luxir -l -b -t rv64) proti GNU ld
s linker skriptem desky tests/ld_bare/board.ld: tests/run/*.lir se
přeloží (-p) a sestaví vestavěným assemblerem, slinkují s testovacím
runtime tests/c32/rt.c a vstupem tests/ld_bare/start.s oběma linkery.
Porovná se vstupní bod, alokované sekce (adresa, velikost, obsah) a
paměťový obraz nahraný ze segmentů PT_LOAD. Obrazy se nespouštějí (běh
v qemu-system ověřují testy cíle baremetal-riscv64 v LSL).

Použití: ld_bare.py --backend LUXIR [--backend …]
"""
import argparse, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
GNU_LD = 'riscv64-linux-gnu-ld'
CFLAGS = ['--target=riscv64-linux-gnu', '-march=rv64g', '-mabi=lp64d', '-mno-relax', '-mcmodel=medany', '-fno-pic']


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def image(path):
    """(vstupní bod, {sekce: (adresa, velikost, obsah)}, paměť od nejnižšího PT_LOAD)"""
    d = path.read_bytes()
    entry, phoff, shoff = struct.unpack_from('<QQQ', d, 24)
    phent, phnum, shent, shnum, shstr = struct.unpack_from('<HHHHH', d, 54)
    sh = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shent) for i in range(shnum)]
    names = sh[shstr][4]
    secs = {}
    for name, typ, flags, addr, off, size, *_ in sh:
        if flags & 2:
            key = d[names + name:d.index(b'\0', names + name)].decode()
            secs[key] = (addr, size, None if typ == 8 else d[off:off + size])
    loads = []
    for i in range(phnum):
        typ, _, off, va, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phent)
        if typ == 1:
            loads.append((va, d[off:off + filesz], memsz))
    low = min(v for v, _, _ in loads)
    mem = bytearray(max(v + m for v, _, m in loads) - low)
    for v, data, _ in loads:
        mem[v - low:v - low + len(data)] = data
    return entry, secs, (low, bytes(mem))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', action='append', required=True)
    a = ap.parse_args()
    if not shutil.which(GNU_LD):
        print(f'ld_bare: {GNU_LD} not found, skipped')
        return 0
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        rt = tmp / 'rt.o'
        r = run(['clang'] + CFLAGS + ['-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-w', '-c', '-o', rt, ROOT / 'c32' / 'rt.c'])
        if r.returncode:
            print(r.stderr[:800])
            return 1
        for backend in a.backend:
            backend = str(Path(backend).resolve())
            start = tmp / 'start.o'
            r = run([backend, '-a', '-t', 'rv64', '-o', start, ROOT / 'ld_bare' / 'start.s'])
            if r.returncode:
                print(f'FAIL    start.s: {r.stderr.strip()[:300]}')
                failed += 1
                continue
            for lir in sorted((ROOT / 'run').glob('*.lir')):
                if lir.stem == 'tls':
                    continue
                name = f'bare-{lir.stem}'
                s, o, gnu, own = tmp / 'p.s', tmp / 'p.o', tmp / 'gnu', tmp / 'own'
                r = run([backend, '-p', '-t', 'rv64', '-o', s, lir])
                if r.returncode == 0:
                    r = run([backend, '-a', '-t', 'rv64', '-o', o, s])
                objs = [start, o, rt]
                if r.returncode == 0:
                    r = run([GNU_LD, '--no-relax', '-nostdlib', '-static', '-T', ROOT / 'ld_bare' / 'board.ld', '-e', '_bare_start',
                             '--build-id=none', '--gc-sections', '-s', '-o', gnu] + objs)
                if r.returncode == 0:
                    r = run([backend, '-l', '-b', '-e', '_bare_start', '-t', 'rv64', '-o', own] + objs)
                if r.returncode:
                    print(f'FAIL    {name}: {r.stderr.strip()[:400]}')
                    failed += 1
                    continue
                ge, gs, gm = image(gnu)
                oe, os_, om = image(own)
                diff = []
                if ge != oe:
                    diff.append(f'entry {ge:#x} != {oe:#x}')
                for sec in sorted(set(gs) | set(os_)):
                    if gs.get(sec) != os_.get(sec):
                        g, w = gs.get(sec), os_.get(sec)
                        diff.append(f'{sec}: {g and (hex(g[0]), hex(g[1]))} != {w and (hex(w[0]), hex(w[1]))}')
                if gm != om:
                    diff.append('memory image')
                if diff:
                    print(f'FAIL    {name}: ' + '; '.join(diff))
                    failed += 1
                else:
                    print(f'ok      {name}')
    print(f'ld_bare: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
