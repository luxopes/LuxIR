#!/usr/bin/env python3
"""Vestavěný assembler WebAssembly (luxir -a -t wasm32) proti wat2wasm
(WABT): každý vstup .wat se sestaví oběma a binární moduly se porovnají
bajt po bajtu (wat2wasm --enable-threads --no-check: validace není
předmětem testu). S --probes navíc modul se všemi instrukcemi tabulky
assembleru (src/wasm/wat.c) s okamžitými operandy podle druhu. Chybné
vstupy musí skončit nenulovým stavem s hlášením a bez výstupu.

Použití: wat_diff.py --backend LUXIR [--backend …] [--probes] soubor.wat …
"""
import argparse, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WAT2WASM = ['wat2wasm', '--enable-threads', '--no-check']

# okamžité operandy podle druhu z tabulky instrukcí
OPERANDS = {
    '-': [''], 'l': ['0', '1'], 't': ['0', '0 1 0'], 'c': ['0', '$f'], 'i': ['(type 0)', '$t (type $sig)'],
    'L': ['0', '$p', '1'], 'G': ['0', '$g'], '4': ['0', '-1', '127', '128', '-129', '0x7fffffff', '-2147483648', '4294967295'],
    '8': ['0', '-64', '63', '0xffffffffffffffff', '-9223372036854775808', '1_000'], 'z': [''], 'Z': [''],
    'D': ['0', '$d'], 'x': ['0', '$d'],
}

BAD = [
    ('undefined function', '(func call $missing)'),
    ('unknown instruction', '(func i32.frobnicate)'),
    ('float constant', '(func f64.const 1.5 drop)'),
    ('unbalanced', '(func'),
    ('import after definition', '(func) (import "a" "b" (func))'),
]


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def probes():
    """Modul s každou instrukcí tabulky assembleru."""
    text = (ROOT.parent / 'src/wasm/wat.c').read_text()
    table = ''.join(re.findall(r'"([^"]*)"', text.split('optext[] =', 1)[1].split(';\n', 1)[0]))
    lines = []
    for entry in filter(None, table.split(';')):
        name, _, kind = entry.split(' ')
        if kind in ('b', 'e', 'n', 'f'):
            continue
        if kind[0] == 'm':
            for imm in ['', 'offset=0', 'offset=7', 'offset=4294967295', 'align=1', 'offset=16 align=' + str(1 << int(kind[1]))]:
                lines.append(f'{name} {imm}'.rstrip())
            continue
        for imm in OPERANDS[kind]:
            lines.append(f'{name} {imm}'.rstrip())
    body = '\n'.join('    ' + l for l in lines)
    return f'''(module
  (type $sig (func (param i32) (result i32)))
  (memory 1 2 shared)
  (table $t 1 funcref)
  (global $g (mut i32) (i32.const 0))
  (data $d "x")
  (func $f (param $p i32) (param i64) (result i32)
    block $outer
    loop $l (result i32)
    i32.const 0
    if (result i32)
      i32.const 1
    else
      i32.const 2
    end
{body}
    end
    drop
    end
    i32.const 0)
)
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', action='append', required=True)
    ap.add_argument('--probes', action='store_true')
    ap.add_argument('files', nargs='*', type=Path)
    a = ap.parse_args()
    if not shutil.which('wat2wasm'):
        print('wat_diff: wat2wasm not found, skipped')
        return 0
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        inputs = list(a.files)
        if a.probes:
            probe = tmp / 'probe_wasm.wat'
            probe.write_text(probes())
            inputs.append(probe)
        for backend in a.backend:
            backend = str(Path(backend).resolve())
            for wat in inputs:
                ref, own = tmp / 'ref.wasm', tmp / 'own.wasm'
                r = run(WAT2WASM + ['-o', ref, wat])
                if r.returncode:
                    print(f'skip {wat}: wat2wasm: {r.stderr.strip()[:200]}')
                    continue
                r = run([backend, '-a', '-t', 'wasm32', '-o', own, wat])
                if r.returncode:
                    print(f'FAIL {wat}: {r.stderr.strip()[:300]}')
                    failed += 1
                elif ref.read_bytes() != own.read_bytes():
                    a_, b_ = ref.read_bytes(), own.read_bytes()
                    at = next((i for i in range(min(len(a_), len(b_))) if a_[i] != b_[i]), min(len(a_), len(b_)))
                    print(f'FAIL {wat}: differs at byte {at:#x} (sizes {len(a_)}, {len(b_)})')
                    failed += 1
                else:
                    print(f'ok   {wat}')
            for what, text in BAD:
                src, out = tmp / 'bad.wat', tmp / 'bad.wasm'
                src.write_text(text + '\n')
                out.unlink(missing_ok=True)
                r = run([backend, '-a', '-t', 'wasm32', '-o', out, src])
                if r.returncode == 0 or 'luxir: wasm:' not in r.stderr or out.exists():
                    print(f'FAIL error case {what}: status {r.returncode}, {r.stderr.strip()[:200]}')
                    failed += 1
                else:
                    print(f'ok   error case {what}')
    print(f'wat_diff: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
