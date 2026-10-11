#!/usr/bin/env python3
"""Spustitelné testy QBE 1.3 přeložené backendem LuxIR.

Odpovídá upstream tools/test.sh: každý tests/upstream/[!_]*.ssa se převede
na LuxIR, přeloží pro zvolený cíl, slinkuje s případným C driverem
z komentáře "# >>> driver" a spustí. Výstup se porovná s "# >>> output"
nebo se kontroluje návratový kód 0. Drivery testují ABI proti skutečnému
C compileru cíle.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import qbe2lir  # noqa: E402

# cíl: (linker, spouštěč) podle upstream tools/test.sh
TARGETS = {
    'amd64_sysv': (['cc'], [], ['-lpthread']),
    'amd64_win': (['x86_64-w64-mingw32-gcc', '-static'], ['wine'], []),
    'arm64': (['aarch64-linux-gnu-gcc', '-no-pie', '-static'], ['qemu-aarch64'], []),
    'rv64': (['riscv64-linux-gnu-gcc', '-no-pie', '-static'], ['qemu-riscv64'], []),
}


def extract(what, text):
    out, on = [], False
    for line in text.splitlines():
        if line.startswith(f'# >>> {what}'):
            on = True
            continue
        if line.startswith('# <<<'):
            on = False
        elif on:
            line = line.replace('# ', '', 1)
            if line.endswith('#'):
                line = line[:-1]
            out.append(line)
    return '\n'.join(out) + ('\n' if out else '')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', type=Path, required=True)
    ap.add_argument('--target', default='amd64_sysv', choices=sorted(TARGETS))
    args = ap.parse_args()
    linker, runner, post = TARGETS[args.target]
    backend = args.backend.resolve()
    env = dict(os.environ, WINEDEBUG='-all')
    counts = {'ok': 0, 'skip': 0, 'fail': 0}
    with tempfile.TemporaryDirectory(prefix='luxir-exec-') as tmp:
        tmp = Path(tmp)
        for ssa in sorted((ROOT / 'tests/upstream').glob('[!_]*.ssa')):
            text = ssa.read_text()
            first = text.splitlines()[0] if text else ''
            if re.search(rf'skip.* {args.target}( .*)?$', first):
                counts['skip'] += 1
                continue
            name = ssa.stem
            lir = tmp / f'{name}.lir'
            asm = tmp / f'{name}.s'
            exe = tmp / f'{name}.exe'
            lir.write_text(qbe2lir.convert(text))
            r = subprocess.run([str(backend), '-t', args.target, '-o', str(asm), str(lir)],
                               capture_output=True)
            if r.returncode != 0:
                print(f'FAIL {name}: backend: {r.stderr.decode()[:200]}')
                counts['fail'] += 1
                continue
            sources = [str(asm)]
            driver = extract('driver', text)
            if driver.strip():
                drv = tmp / f'{name}_drv.c'
                drv.write_text(driver)
                sources.insert(0, str(drv))
            r = subprocess.run(linker + ['-g', '-o', str(exe)] + sources + post,
                               capture_output=True)
            if r.returncode != 0:
                print(f'FAIL {name}: cc: {r.stderr.decode()[:300]}')
                counts['fail'] += 1
                continue
            r = subprocess.run(runner + [str(exe), 'a', 'b', 'c'], capture_output=True,
                               timeout=300, env=env)
            expected = extract('output', text)
            got = r.stdout.decode('utf-8', 'replace').replace('\r', '')
            if expected:
                ok = got == expected
                reason = 'output'
            else:
                ok = r.returncode == 0
                reason = f'returned {r.returncode}'
            if ok:
                counts['ok'] += 1
            else:
                counts['fail'] += 1
                print(f'FAIL {name}: {reason}')
    print(f'{args.target}: {counts}')
    return 1 if counts['fail'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
