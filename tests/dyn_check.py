#!/usr/bin/env python3
"""Dynamicky linkovaný program (luxir -l se vstupem .libs, x86-64): tests/run/*.lir
se přeloží, sestaví a slinkují proti libc.so.6 (printf, puts a exit skutečné
glibc, vstup _start v tests/dyn/start.s) a spustí přímo na hostiteli; výstup
se porovná s .out. Kontroluje se i smlouva obrazu: ET_DYN s PT_INTERP
/lib64/ld-linux-x86-64.so.2 a PT_DYNAMIC, DT_NEEDED libc.so.6, BIND_NOW,
žádný RWX segment a nespustitelný zásobník.

Použití: dyn_check.py --backend LUXIR [--backend …]
"""
import argparse, platform, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, **kw)


def contract(path):
    d = path.read_bytes()
    assert d[:4] == b'\x7fELF' and d[4] == 2
    e_type, = struct.unpack_from('<H', d, 16)
    assert e_type == 3, 'ET_DYN'
    phoff, = struct.unpack_from('<Q', d, 32)
    phentsize, phnum = struct.unpack_from('<HH', d, 54)
    kinds, interp, dynamic = {}, None, None
    for i in range(phnum):
        p = phoff + i * phentsize
        ptype, flags, off, va, pa, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', d, p)
        kinds[ptype] = kinds.get(ptype, 0) + 1
        if ptype == 3:
            interp = d[off:off + filesz].rstrip(b'\0')
        if ptype == 2:
            dynamic = d[off:off + filesz]
        if ptype == 1:
            assert flags & 3 != 3, 'RWX segment'
        if ptype == 0x6474e551:
            assert flags & 1 == 0, 'executable stack'
    assert interp == b'/lib64/ld-linux-x86-64.so.2', interp
    assert dynamic, 'PT_DYNAMIC'
    tags = {}
    for i in range(0, len(dynamic), 16):
        tag, val = struct.unpack_from('<QQ', dynamic, i)
        tags.setdefault(tag, []).append(val)
        if tag == 0:
            break
    assert 1 in tags and 4 in tags and 5 in tags and 6 in tags and 7 in tags, 'dynamic tags'
    assert tags[30] == [8] and tags[0x6ffffffb] == [1 | 0x08000000], 'BIND_NOW, PIE'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', action='append', required=True)
    a = ap.parse_args()
    if platform.machine() != 'x86_64' or not Path('/lib64/ld-linux-x86-64.so.2').exists():
        print('dyn_check: not a glibc x86-64 host, skipped')
        return 0
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        libs = tmp / 'libc.libs'
        libs.write_text('# glibc\nlibc.so.6\n')
        for backend in a.backend:
            backend = str(Path(backend).resolve())
            start = tmp / 'start.o'
            r = run([backend, '-a', '-t', 'amd64_sysv', '-o', start, ROOT / 'dyn' / 'start.s'], text=True)
            if r.returncode:
                print(f'FAIL    start.s: {r.stderr.strip()[:300]}')
                failed += 1
                continue
            for lir in sorted((ROOT / 'run').glob('*.lir')):
                if lir.stem in ('tls', 'memory'):   # memory: absolutní adresy v datech
                    continue
                name = f'dyn-{lir.stem}'
                s, o, exe = tmp / 'p.s', tmp / 'p.o', tmp / 'p'
                r = run([backend, '-t', 'amd64_sysv', '-o', s, lir], text=True)
                if r.returncode == 0:
                    r = run([backend, '-a', '-t', 'amd64_sysv', '-o', o, s], text=True)
                if r.returncode == 0:
                    r = run([backend, '-l', '-p', '-t', 'amd64_sysv', '-o', exe, o, start, libs], text=True)
                if r.returncode:
                    print(f'FAIL    {name}: {r.stderr.strip()[:400]}')
                    failed += 1
                    continue
                try:
                    contract(exe)
                except AssertionError as e:
                    print(f'FAIL    {name}: image contract: {e}')
                    failed += 1
                    continue
                r = run([exe], timeout=60)
                expected = lir.with_suffix('.out').read_bytes()
                if r.returncode or r.stdout != expected:
                    print(f'FAIL    {name}: status {r.returncode}')
                    print('  got     ', r.stdout[:300])
                    print('  expected', expected[:300])
                    failed += 1
                    continue
                # strip (BFD skládá soubor ze sekcí): interpret a tabulky ld.so
                # mají vlastní sekce, takže program běží i po strip/objcopy
                stripped = tmp / 'p-stripped'
                if shutil.which('strip'):
                    shutil.copy(exe, stripped)
                    r = run(['strip', stripped], text=True)
                    if r.returncode == 0:
                        r = run([stripped], timeout=60)
                    if r.returncode or r.stdout != expected:
                        print(f'FAIL    {name}: after strip: status {r.returncode} {getattr(r, "stderr", b"")[:200]}')
                        failed += 1
                        continue
                print(f'ok      {name}')
    print(f'dyn_check: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
