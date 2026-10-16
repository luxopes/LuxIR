#!/usr/bin/env python3
"""Rozdílový test 32bitových cílů (i386, arm) proti amd64: náhodné programy
s 64bitovou aritmetikou, porovnáními, posuny, převody a pamětí se přeloží
pro amd64_sysv (referenční chování, spuštění s libc) a pro 32bitový cíl
(runtime testů tests/c32/rt.c, pomocné funkce runtime/w32/helpers.lir)
a jejich výstupy se musí shodovat.

Operandy jdou z dat, aby je backend nemohl spočítat předem. Dělení se
vyhýbá nule a MIN / -1 (na amd64 nedefinované), převody desetinných čísel
na celá zůstávají v rozsahu.

Použití: w32_diff.py --backend LUXIR [--arch i386|arm] [--cases N] [--seed S]
"""
import argparse, random, subprocess, sys, tempfile
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
M64 = (1 << 64) - 1

SPECIAL = [0, 1, 2, 3, -1, -2, 7, 31, 32, 33, 63, 64, 0x7fffffff, 0x80000000, 0xffffffff,
           0x100000000, -0x80000000, -0x100000000, 0x7fffffffffffffff, -0x8000000000000000,
           0x123456789abcdef0, -0x123456789abcdef, 0xdeadbeef, 1 << 40, -(1 << 40)]


def s64(v):
    v &= M64
    return v - (1 << 64) if v >> 63 else v


def rnd(rng):
    c = rng.random()
    if c < 0.3:
        return s64(rng.choice(SPECIAL))
    if c < 0.5:
        return s64(rng.randrange(-1000, 1000))
    if c < 0.7:
        return s64(rng.getrandbits(32))
    return s64(rng.getrandbits(64))


class Gen:
    def __init__(self, rng):
        self.rng = rng
        self.data = []
        self.lines = []
        self.n = 0
        self.vals = []   # (jméno, hodnota v Pythonu) Int64

    def fresh(self, p='v'):
        self.n += 1
        return f'{p}{self.n}'

    def load(self, value):
        idx = len(self.data)
        self.data.append(s64(value))
        p = self.fresh('p')
        v = self.fresh()
        self.lines.append(f'    Ptr({p}) = add(vals, {idx * 8})')
        self.lines.append(f'    Int64({v}) = load({p})')
        self.vals.append(v)
        return v

    def operand(self):
        r = self.rng.random()
        if r < 0.15:
            return str(s64(rnd(self.rng)))
        if r < 0.6 and self.vals:
            return self.rng.choice(self.vals)
        return self.load(rnd(self.rng))

    def out(self, v, cls='Int64'):
        if cls == 'Int32':
            w = self.fresh()
            self.lines.append(f'    Int64({w}) = sext32({v})')
            v = w
        self.lines.append(f'    call out(Int64({v}))')

    def step(self):
        g = self.rng
        kind = g.choice(['bin', 'bin', 'bin', 'shift', 'cmp', 'cmpjmp', 'div', 'un', 'conv', 'mem', 'w32', 'phi', 'float'])
        a = self.operand()
        b = self.operand()
        v = self.fresh()
        L = self.lines
        if kind == 'bin':
            op = g.choice(['add', 'sub', 'mul', 'and', 'or', 'xor'])
            L.append(f'    Int64({v}) = {op}(Int64({a}), Int64({b}))')
        elif kind == 'shift':
            op = g.choice(['shl', 'shr', 'sar'])
            if g.random() < 0.5:
                amount = str(g.randrange(0, 64))
            else:
                amount = self.fresh()
                src = self.load(rnd(g) & 63 if g.random() < 0.7 else rnd(g))
                L.append(f'    Int32({amount}) = Int32({src})')
            L.append(f'    Int64({v}) = {op}(Int64({a}), {amount})')
        elif kind == 'cmp':
            op = g.choice(['eq', 'ne', 'lt', 'le', 'gt', 'ge', 'ult', 'ule', 'ugt', 'uge'])
            t = g.choice(['Int32', 'Int64'])
            L.append(f'    {t}({v}) = {op}(Int64({a}), Int64({b}))')
            self.out(v, t)
            return
        elif kind == 'cmpjmp':
            op = g.choice(['eq', 'ne', 'lt', 'le', 'gt', 'ge', 'ult', 'ule', 'ugt', 'uge'])
            c = self.fresh('c')
            yes, no, join = self.fresh('L'), self.fresh('L'), self.fresh('L')
            L.append(f'    Int32({c}) = {op}(Int64({a}), Int64({b}))')
            L.append(f'    if {c} goto {yes} else {no}')
            L.append(f'{yes}:')
            L.append(f'    goto {join}')
            L.append(f'{no}:')
            L.append(f'    goto {join}')
            L.append(f'{join}:')
            L.append(f'    Int64({v}) = phi({yes}: 11, {no}: 22)')
        elif kind == 'div':
            op = g.choice(['div', 'rem', 'udiv', 'urem'])
            bv = rnd(g)
            if bv == 0:
                bv = 3
            if bv == -1:
                bv = -3
            b = self.load(bv) if g.random() < 0.7 else str(bv)
            L.append(f'    Int64({v}) = {op}(Int64({a}), Int64({b}))')
        elif kind == 'un':
            op = g.choice(['neg', 'sext8', 'zext8', 'sext16', 'zext16', 'sext32', 'zext32'])
            if op == 'neg':
                L.append(f'    Int64({v}) = neg(Int64({a}))')
            else:
                L.append(f'    Int64({v}) = {op}({a})')
        elif kind == 'conv':
            f = self.fresh('f')
            op = g.choice(['itof', 'utof', 'itof32'])
            if op == 'itof32':
                L.append(f'    Float32({f}) = itof(Int64({a}))')
                d = self.fresh('f')
                L.append(f'    Float64({d}) = fext({f})')
                f = d
            else:
                L.append(f'    Float64({f}) = {op}(Int64({a}))')
            L.append(f'    Int64({v}) = bitcast({f})')
        elif kind == 'float':
            # Float64 z dat -> Int64 v rozsahu, se znaménkem i bez
            fv = g.choice([0.0, 0.5, -0.5, 1.5, -7.25, 123456789.75, -987654321.5, 4294967295.5,
                           4294967296.0, 9.0e15, -9.0e15, 1.0e18, -1.0e18, 9.2e18, 1.8e19])
            idx = len(self.data)
            import struct
            self.data.append(s64(struct.unpack('<q', struct.pack('<d', fv))[0]))
            p, raw, f = self.fresh('p'), self.fresh(), self.fresh('f')
            L.append(f'    Ptr({p}) = add(vals, {idx * 8})')
            L.append(f'    Float64({f}) = load({p})')
            if fv < 0 or fv >= 9.3e18:
                if fv >= 1.8e19 or fv < 0:
                    if fv < 0 and fv > -9.2e18:
                        L.append(f'    Int64({v}) = ftoi({f})')
                    else:
                        L.append(f'    Int64({v}) = 0')
                else:
                    L.append(f'    Int64({v}) = ftou({f})')
            else:
                L.append(f'    Int64({v}) = {g.choice(["ftoi", "ftou"])}({f})')
        elif kind == 'mem':
            p = self.fresh('p')
            L.append(f'    Ptr({p}) = add(scratch, {g.randrange(0, 4) * 8})')
            L.append(f'    store(Int64({a}), {p})')
            op = g.choice(['load', 'load_i32', 'load_u32', 'load_i16', 'load_u8'])
            off = g.choice([0, 0, 4, 2, 1])
            q = self.fresh('p')
            L.append(f'    Ptr({q}) = add({p}, {off})')
            L.append(f'    Int64({v}) = {op}({q})')
        elif kind == 'w32':
            op = g.choice(['add', 'sub', 'mul', 'and', 'xor', 'shl', 'shr', 'sar', 'div', 'udiv', 'rem', 'urem'])
            x, y = self.fresh(), self.fresh()
            L.append(f'    Int32({x}) = Int32({a})')
            bv = rnd(g) & 0xffffffff
            if op in ('div', 'udiv', 'rem', 'urem'):
                if bv in (0, 0xffffffff):
                    bv = 5
                L.append(f'    Int32({y}) = Int32({self.load(bv)})')
                if op in ('div', 'rem') and g.random() < 0.5:
                    pass
            elif op in ('shl', 'shr', 'sar'):
                L.append(f'    Int32({y}) = Int32({self.load(rnd(g))})')
            else:
                L.append(f'    Int32({y}) = Int32({b})')
            w = self.fresh()
            L.append(f'    Int32({w}) = {op}({x}, {y})')
            self.out(w, 'Int32')
            return
        elif kind == 'phi':
            n = self.fresh()
            head, body, done = self.fresh('L'), self.fresh('L'), self.fresh('L')
            start = self.fresh('L')
            k = g.randrange(1, 6)
            L.append(f'    goto {start}')
            L.append(f'{start}:')
            L.append(f'    goto {head}')
            L.append(f'{head}:')
            acc, i, acc1, i1 = self.fresh(), self.fresh(), self.fresh(), self.fresh()
            L.append(f'    Int64({acc}) = phi({start}: {a}, {body}: {acc1})')
            L.append(f'    Int64({i}) = phi({start}: 0, {body}: {i1})')
            c = self.fresh('c')
            L.append(f'    Int32({c}) = lt({i}, {k})')
            L.append(f'    if {c} goto {body} else {done}')
            L.append(f'{body}:')
            t = self.fresh()
            L.append(f'    Int64({t}) = {g.choice(["add", "mul", "xor", "sub"])}({acc}, Int64({b}))')
            L.append(f'    Int64({acc1}) = shl({t}, 1)')
            L.append(f'    Int64({i1}) = add({i}, 1)')
            L.append(f'    goto {head}')
            L.append(f'{done}:')
            L.append(f'    Int64({v}) = {acc}')
        self.vals.append(v)
        self.out(v)

    def program(self, steps):
        for _ in range(steps):
            self.step()
        data = ', '.join(str(d) for d in self.data) or '0'
        return '\n'.join([
            'data fmt = ["%lld\\n", Int8(0)]',
            f'export data vals = [Int64({data})]',
            'export data scratch = [zero(64)]',
            'function out(Int64(v)):',
            '    call printf(Ptr(fmt), ..., Int64(v))',
            '    return',
            'end',
            'export function main() -> Int32:',
        ] + self.lines + ['    return 0', 'end', ''])


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', required=True)
    ap.add_argument('--arch', action='append', choices=sorted(ARCHES))
    ap.add_argument('--cases', type=int, default=40)
    ap.add_argument('--steps', type=int, default=60)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--keep', type=Path, help='při chybě sem uloží program')
    a = ap.parse_args()
    backend = str(Path(a.backend).resolve())
    rng = random.Random(a.seed)
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        prep = {}
        for arch in a.arch or sorted(ARCHES):
            asm, cflags, ldflags, runner = ARCHES[arch]
            objs = []
            for name, source in [('rt', ROOT / 'c32' / 'rt.c'), ('builtins', BUILTINS)]:
                obj = tmp / f'{arch}-{name}.o'
                r = run(['clang'] + cflags + ['-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-w', '-c', '-o', obj, source], text=True)
                if r.returncode:
                    print(r.stderr)
                    return 1
                objs.append(obj)
            hs, ho = tmp / f'{arch}-h.s', tmp / f'{arch}-h.o'
            r = run([backend, '-t', arch, '-o', hs, HELPERS], text=True)
            if r.returncode or run(asm + ['-o', ho, hs]).returncode:
                print(f'{arch}: helpers failed: {r.stderr}')
                return 1
            objs.append(ho)
            prep[arch] = objs
        for case in range(a.cases):
            text = Gen(rng).program(a.steps)
            lir = tmp / f'c{case}.lir'
            lir.write_text(text)
            r = run([backend, '-t', 'amd64_sysv', '-o', tmp / 'ref.s', lir], text=True)
            if r.returncode:
                print(f'case {case}: amd64 backend: {r.stderr[:300]}')
                failed += 1
                continue
            r = run(['cc', '-o', tmp / 'ref', tmp / 'ref.s'], text=True)
            if r.returncode:
                print(f'case {case}: cc: {r.stderr[:300]}')
                failed += 1
                continue
            ref = run([tmp / 'ref']).stdout
            for arch, objs in prep.items():
                asm, cflags, ldflags, runner = ARCHES[arch]
                s, o, exe = tmp / f'{arch}.s', tmp / f'{arch}.o', tmp / f'{arch}.exe'
                r = run([backend, '-t', arch, '-o', s, lir], text=True)
                if r.returncode == 0:
                    r = run(asm + ['-o', o, s], text=True)
                if r.returncode == 0:
                    r = run(['ld.lld'] + ldflags + ['-static', '-o', exe, o] + objs, text=True)
                if r.returncode:
                    print(f'case {case} {arch}: build: {r.stderr[:800]}')
                    failed += 1
                    continue
                got = run(runner + [exe], timeout=60)
                if got.stdout != ref or got.returncode:
                    failed += 1
                    rl, gl = ref.decode().split('\n'), got.stdout.decode().split('\n')
                    first = next((n for n in range(max(len(rl), len(gl))) if (rl[n:n+1] != gl[n:n+1])), None)
                    print(f'case {case} {arch}: mismatch at output line {first} (status {got.returncode})')
                    if first is not None:
                        print(f'  amd64: {rl[first:first+1]}  {arch}: {gl[first:first+1]}')
                    if a.keep:
                        a.keep.mkdir(parents=True, exist_ok=True)
                        (a.keep / f'case{case}.lir').write_text(text)
    print(f'w32 diff: {a.cases} cases, {failed} failures')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
