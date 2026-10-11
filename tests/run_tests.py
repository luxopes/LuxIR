#!/usr/bin/env python3
"""Testy jazyka LuxIR: spustitelné programy a chybová hlášení.

Programy v tests/run/*.lir se přeloží backendem, slinkují s libc a jejich
výstup se porovná s tests/run/*.out. Výchozí cíl amd64_sysv se linkuje
systémovým cc; amd64_win se linkuje mingw-w64 a spouští pod Wine, arm64
a rv64 se linkují cross gcc a spouštějí v qemu-user.
Chybové testy ověří, že backend vstup odmítne s očekávaným hlášením.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent

# (jméno, zdroj, očekávaná část hlášení)
ERRORS = [
    ('unknown-instruction', 'function f():\n    Int32(x) = foo(1)\n    return\nend\n',
     'unknown instruction foo'),
    ('op-without-result', 'function f():\n    add(1, 2)\n    return\nend\n',
     'add needs a result'),
    ('untyped-call-literal', 'function f():\n    call g(1)\n    return\nend\n',
     'call argument needs a type'),
    ('int-in-float', 'function f():\n    Float64(x) = add(1.0, 2)\n    return\nend\n',
     'write a decimal point'),
    ('float-in-int', 'function f():\n    Int32(x) = add(1, 2.5)\n    return\nend\n',
     'floating literal 2.5 used as an integer'),
    ('cannot-infer', 'function f():\n    Int32(x) = lt(1, 2)\n    return\nend\n',
     'cannot infer the operand type of lt'),
    ('unsigned-float', 'function f(Float64(a)):\n    Int32(x) = ult(a, 1.0)\n    return\nend\n',
     'ult needs integer operands'),
    ('ord-int', 'function f(Int32(a)):\n    Int32(x) = ord(a, 1)\n    return\nend\n',
     'ord needs floating operands'),
    ('after-jump', 'function f() -> Int32:\n    return 0\n    Int32(x) = 1\nend\n',
     'label or end expected after a jump'),
    ('undefined-block', 'function f():\n    goto nowhere\nend\n',
     'block nowhere is used undefined'),
    ('late-phi', 'function f():\nstart:\n    goto b\nb:\n    Int32(y) = 1\n'
     '    Int32(x) = phi(start: 1)\n    return\nend\n', 'unexpected phi instruction'),
    ('phi-in-entry', 'function f():\n    Int32(x) = phi(a: 1)\n    return\nend\n',
     'unexpected phi instruction'),
    ('untyped-store', 'function f(Ptr(p)):\n    store(0, p)\n    return\nend\n',
     'store needs a typed value'),
    ('missing-end', 'function f():\n    return\n', 'missing end of function'),
    ('bad-escape', 'data s = "a\\qb"\n', 'invalid escape sequence \\q'),
    ('unterminated-string', 'data s = "abc\n', 'unterminated string'),
    ('reserved-type', 'type Int32 = struct(Int8)\n', 'type name Int32 is reserved'),
    ('opaque-align', 'type B = opaque(8)\n', 'opaque types need alignment'),
    ('multiple-types', 'function f():\n    Int32(x) = 1\n    Float64(x) = 2.0\n    return\nend\n',
     'value x is assigned with multiple types'),
    ('vastart-fixed', 'function f(Ptr(p)):\n    vastart(p)\n    return\nend\n',
     'cannot use vastart in non-variadic function'),
    ('bad-param', 'function f(Word(x)):\n    return\nend\n', 'unknown parameter type Word'),
    ('aggregate-copy', 'type P = struct(Int64)\nfunction f():\n    P(x) = 1\n    return\nend\n',
     'aggregate type is only valid for call results'),
    ('jump-start', 'function f():\nstart:\n    goto start\nend\n',
     'invalid jump to the start block'),
    ('missing-operand', 'function f():\n    Int32(x) = add(1)\n    return\nend\n',
     'missing second operand in add'),
    ('nul-byte', 'function f():\n    return\x00\nend\n', 'NUL byte in input'),
    ('data-int-float', 'data d = Float64(1)\n', 'write a decimal point'),
    ('data-float-int', 'data d = Int32(1.5)\n', 'floating literal 1.5 used as an integer'),
    ('small-result', 'function f():\n    Int8(x) = add(1, 2)\n    return\nend\n',
     'result of an instruction must be'),
    ('wrong-operand-type', 'function f(Int32(a)):\n    Int64(x) = add(a, 1)\n    return\nend\n',
     'invalid type for first operand a in add'),
    ('local-thread', 'function f(Ptr(p)):\n    Int32(x) = load(thread(p))\n    return\nend\n',
     'thread(...) needs a global symbol'),
    ('end-trailing', 'function f():\n    return\nend x\n', 'newline expected after end'),
    ('bad-number', 'function f():\n    Int32(x) = 0x\n    return\nend\n',
     'digits expected after number prefix'),
    ('if-without-else', 'function f(Int32(c)):\n    if c goto a\na:\n    return\nend\n',
     'else expected'),
    ('two-envs', 'function f(Env(a), Env(b)):\n    return\nend\n', 'only one environment allowed'),
    ('after-dots', 'function f(Int32(a), ..., Int32(b)):\n    return\nend\n',
     "no parameters allowed after '...'"),
    ('thread-function', 'thread function f():\n    return\nend\n',
     'only data may have thread linkage'),
]


# cíle, které spustitelné testy umí slinkovat a spustit na tomto hostu
TARGETS = {
    'amd64_sysv': (['cc'], [], ''),
    'amd64_win': (['x86_64-w64-mingw32-gcc'], ['wine'], '.exe'),
    'arm64': (['aarch64-linux-gnu-gcc'], ['qemu-aarch64', '-L', '/usr/aarch64-linux-gnu'], ''),
    # QBE generuje pro rv64 kód bez PIC, proto -no-pie
    'rv64': (['riscv64-linux-gnu-gcc', '-no-pie'], ['qemu-riscv64', '-L', '/usr/riscv64-linux-gnu'], ''),
}
# programy, které backend pro daný cíl záměrně odmítne
UNSUPPORTED = {('amd64_win', 'tls'): 'thread-local data is not supported'}


def run_programs(backend, tmp, target):
    failures = 0
    linker, runner, suffix = TARGETS[target]
    for source in sorted((ROOT / 'run').glob('*.lir')):
        expected = source.with_suffix('.out').read_bytes()
        refused = UNSUPPORTED.get((target, source.stem))
        asm = tmp / (source.stem + '.s')
        exe = tmp / (source.stem + suffix)
        r = subprocess.run([str(backend), '-t', target, '-o', str(asm), str(source)],
                           capture_output=True)
        if refused:
            if r.returncode != 0 and refused in r.stderr.decode():
                print(f'ok   {source.name} (refused: {refused})')
            else:
                print(f'FAIL {source.name}: expected refusal "{refused}"')
                failures += 1
            continue
        if r.returncode != 0:
            print(f'FAIL {source.name}: backend: {r.stderr.decode().strip()}')
            failures += 1
            continue
        r = subprocess.run(linker + ['-o', str(exe), str(asm)], capture_output=True)
        if r.returncode != 0:
            print(f'FAIL {source.name}: link: {r.stderr.decode().strip()[:500]}')
            failures += 1
            continue
        env = dict(os.environ, WINEDEBUG='-all')
        r = subprocess.run(runner + [str(exe)], capture_output=True, timeout=120, env=env)
        out = r.stdout.replace(b'\r\n', b'\n') if target == 'amd64_win' else r.stdout
        if r.returncode != 0 or out != expected:
            print(f'FAIL {source.name}: status {r.returncode}')
            print('  expected:', expected[:400])
            print('  got:     ', out[:400])
            failures += 1
        else:
            print(f'ok   {source.name}')
    return failures


def run_errors(backend, tmp):
    failures = 0
    for name, text, message in ERRORS:
        path = tmp / f'{name}.lir'
        path.write_bytes(text.encode())
        r = subprocess.run([str(backend), str(path)], capture_output=True)
        err = r.stderr.decode('utf-8', 'replace')
        if r.returncode == 0 or message not in err:
            print(f'FAIL error/{name}: status {r.returncode}: {err.strip()[:300]}')
            failures += 1
        elif not err.startswith('luxir:'):
            print(f'FAIL error/{name}: message without luxir: prefix: {err.strip()[:300]}')
            failures += 1
    print(f'{len(ERRORS) - failures}/{len(ERRORS)} error cases ok')
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', type=Path, action='append', required=True)
    ap.add_argument('--target', default='amd64_sysv', choices=sorted(TARGETS))
    args = ap.parse_args()
    failures = 0
    for backend in args.backend:
        print(f'== {backend}')
        with tempfile.TemporaryDirectory(prefix='luxir-run-') as tmp:
            tmp = Path(tmp)
            failures += run_programs(backend.resolve(), tmp, args.target)
            failures += run_errors(backend.resolve(), tmp)
    print('FAILED' if failures else 'all tests passed')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
