#!/usr/bin/env python3
"""Mutační fuzzing čtečky LuxIR.

Náhodně poškozené vstupy (z tests/run, examples a převedeného korpusu)
nesmí backend shodit: očekává se buď úspěch, nebo hlášení s prefixem
"luxir:" či známá interní kontrola QBE. Volitelně se výsledek porovná
s druhou implementací (C proti native LSL).
"""
import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import qbe2lir  # noqa: E402

TOKENS = [b'(', b')', b'[', b']', b',', b':', b'=', b'\n', b' ', b'"', b'\\', b'-', b'...',
          b'Int32', b'Int64', b'Ptr', b'Float64', b'Int8', b'Env', b'end', b'goto', b'if',
          b'else', b'return', b'call', b'phi', b'store', b'blit', b'0', b'-1', b'1.5',
          b'0x', b'bits(', b'thread(', b'function', b'data', b'type', b'\x00', b'#']


def mutate(rng, data):
    data = bytearray(data)
    for _ in range(rng.randint(1, 4)):
        op = rng.randrange(4)
        pos = rng.randrange(len(data) + 1)
        if op == 0 and data:
            del data[pos:pos + rng.randint(1, 8)]
        elif op == 1:
            data[pos:pos] = rng.choice(TOKENS)
        elif op == 2 and data:
            data[min(pos, len(data) - 1)] = rng.randrange(256)
        else:
            data = data[:pos]
    return bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', type=Path, required=True)
    ap.add_argument('--compare', type=Path)
    ap.add_argument('--cases', type=int, default=1000)
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    seeds = [p.read_bytes() for p in sorted((ROOT / 'tests/run').glob('*.lir'))]
    seeds += [p.read_bytes() for p in sorted((ROOT / 'examples').glob('*.lir'))]
    for p in sorted((ROOT / 'tests/upstream').glob('*.ssa'))[:30]:
        seeds.append(qbe2lir.convert(p.read_text()).encode())
    bad = 0
    stats = {'ok': 0, 'error': 0, 'internal': 0}
    with tempfile.TemporaryDirectory(prefix='luxir-fuzz-') as tmp:
        path = Path(tmp) / 'case.lir'
        for n in range(args.cases):
            data = mutate(rng, rng.choice(seeds))
            path.write_bytes(data)
            r = subprocess.run([str(args.backend), '-o', '/dev/null', str(path)],
                               capture_output=True, timeout=60)
            err = r.stderr.decode('utf-8', 'replace')
            if r.returncode == 0:
                stats['ok'] += 1
            elif err.startswith('luxir:'):
                stats['error'] += 1
            elif 'Sanitizer' in err or 'runtime error' in err or r.returncode < 0:
                bad += 1
                keep = ROOT / 'build' / f'fuzz-crash-{n}.lir'
                keep.parent.mkdir(exist_ok=True)
                keep.write_bytes(data)
                print(f'CRASH case {n} (saved {keep}): status {r.returncode}: {err[:300]}')
                continue
            else:
                # kontroly průchodů QBE (die/assert) po úspěšném parsování
                stats['internal'] += 1
            if args.compare:
                c = subprocess.run([str(args.compare), '-o', '/dev/null', str(path)],
                                   capture_output=True, timeout=60)
                if (c.returncode == 0) != (r.returncode == 0) or c.stderr != r.stderr:
                    bad += 1
                    keep = ROOT / 'build' / f'fuzz-diff-{n}.lir'
                    keep.parent.mkdir(exist_ok=True)
                    keep.write_bytes(data)
                    print(f'DIFF case {n} (saved {keep}): {r.returncode}/{c.returncode}'
                          f' {err[:150]!r} vs {c.stderr[:150]!r}')
    print(f'{args.cases} cases: {stats}, {bad} problems')
    return 1 if bad else 0


if __name__ == '__main__':
    raise SystemExit(main())
