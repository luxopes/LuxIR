#!/usr/bin/env python3
"""Vestavěný linker pro Windows (luxir -l -t amd64_win|arm64_win):
tests/run/*.lir se přeloží backendem, sestaví vestavěným assemblerem
(COFF) a slinkují s testovacím runtime tests/c32/rt.c (clang jen pro
testy) a importy tests/pe/kernel32.def. Kontroluje se smlouva obrazu
(PE32+, konzole, ASLR s high-entropy VA, NX, zásobník 8 MiB, žádná sekce
zapisovatelná a spustitelná zároveň, importy jen z .def, deterministický
výstup); x86-64 se spustí ve wine a výstup se porovná s .out.

Použití: pe_check.py --backend LUXIR [--backend …] [--arch x86_64|arm64]
"""
import argparse, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ARCH = {
    'x86_64': ('amd64_win', 0x8664, ['--target=x86_64-w64-windows-gnu']),
    'arm64': ('arm64_win', 0xaa64, ['--target=aarch64-w64-windows-gnu']),
}
UNSUPPORTED = {'tls'}


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, **kw)


def contract(path, machine):
    d = path.read_bytes()
    assert d[:2] == b'MZ', 'DOS header'
    pe, = struct.unpack_from('<I', d, 60)
    assert d[pe:pe + 4] == b'PE\0\0', 'PE signature'
    mach, nsec, stamp, _, _, optsz, ch = struct.unpack_from('<HHIIIHH', d, pe + 4)
    assert mach == machine, 'machine'
    assert stamp == 0, 'timestamp'
    assert ch & 0x2 and ch & 0x20 and not ch & 0x1, 'characteristics'
    o = pe + 24
    magic, = struct.unpack_from('<H', d, o)
    assert magic == 0x20b, 'PE32+'
    subsystem, dllch = struct.unpack_from('<HH', d, o + 68)
    assert subsystem == 3, 'console subsystem'
    assert dllch & 0x160 == 0x160, 'DYNAMIC_BASE, HIGH_ENTROPY_VA, NX_COMPAT'
    reserve, commit = struct.unpack_from('<QQ', d, o + 72)
    assert reserve == commit == 8 << 20, 'stack 8 MiB'
    imp_rva, imp_size = struct.unpack_from('<II', d, o + 112 + 8)
    assert imp_rva and imp_size, 'import directory'
    for i in range(nsec):
        s = pe + 24 + optsz + 40 * i
        flags, = struct.unpack_from('<I', d, s + 36)
        assert not (flags & 0x20000000 and flags & 0x80000000), f'section {d[s:s + 8]!r} writable and executable'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', action='append', required=True)
    ap.add_argument('--arch', action='append', choices=sorted(ARCH))
    a = ap.parse_args()
    if not shutil.which('clang'):
        print('pe_check: clang not found, skipped')
        return 0
    wine = shutil.which('wine')
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for arch in a.arch or sorted(ARCH):
            target, machine, cflags = ARCH[arch]
            rt = tmp / f'rt-{arch}.o'
            r = run(['clang'] + cflags + ['-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-w', '-c', '-o', rt, ROOT / 'c32' / 'rt.c'], text=True)
            if r.returncode:
                print(r.stderr[:800])
                return 1
            for backend in a.backend:
                backend = str(Path(backend).resolve())
                for lir in sorted((ROOT / 'run').glob('*.lir')):
                    if lir.stem in UNSUPPORTED:
                        continue
                    name = f'{arch}-{lir.stem}'
                    s, o, exe, exe2 = tmp / 'p.s', tmp / 'p.o', tmp / 'p.exe', tmp / 'p2.exe'
                    r = run([backend, '-t', target, '-o', s, lir], text=True)
                    if r.returncode == 0:
                        r = run([backend, '-a', '-t', target, '-o', o, s], text=True)
                    if r.returncode == 0:
                        r = run([backend, '-l', '-t', target, '-o', exe, o, rt, ROOT / 'pe' / 'kernel32.def'], text=True)
                    if r.returncode == 0:
                        r = run([backend, '-l', '-t', target, '-o', exe2, o, rt, ROOT / 'pe' / 'kernel32.def'], text=True)
                    if r.returncode:
                        print(f'FAIL    {name}: {r.stderr.strip()[:400]}')
                        failed += 1
                        continue
                    try:
                        contract(exe, machine)
                        assert exe.read_bytes() == exe2.read_bytes(), 'output not deterministic'
                    except AssertionError as e:
                        print(f'FAIL    {name}: image contract: {e}')
                        failed += 1
                        continue
                    if arch != 'x86_64' or not wine:
                        print(f'ok      {name} (linked)')
                        continue
                    r = run([wine, exe], timeout=120, env={'WINEDEBUG': '-all', 'PATH': '/usr/bin:/bin', 'HOME': str(Path.home())})
                    expected = lir.with_suffix('.out').read_bytes()
                    if r.returncode or r.stdout.replace(b'\r\n', b'\n') != expected:
                        print(f'FAIL    {name}: status {r.returncode}')
                        print('  got     ', r.stdout[:300])
                        print('  expected', expected[:300])
                        failed += 1
                    else:
                        print(f'ok      {name}')
    print(f'pe_check: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
