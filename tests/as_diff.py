#!/usr/bin/env python3
"""Vestavěný assembler proti GNU as: každý vstup .s se sestaví oběma a
porovná se obsah každé sekce (bajtově), relokace (posun, typ, symbol,
addend) a symboly (jméno, hodnota, velikost, vazba, typ, sekce).

Použití: as_diff.py --backend LUXIR [--backend …] --arch x86_64|i386 soubor.s …
         as_diff.py --backend LUXIR --arch x86_64 --lir [-t cíl] soubor.lir …
S --lir se vstupy nejdřív přeloží backendem na assembler. Vstupy, které
GNU as pro danou architekturu odmítne, se přeskočí.
"""
import argparse, re, struct, subprocess, sys, tempfile
from pathlib import Path

ARCH = {
    'x86_64': (['as', '--64'], 'amd64_sysv', ['--target=x86_64-linux-gnu']),
    'i386': (['as', '--32'], 'i386', ['--target=i686-linux-gnu']),
    'arm64': (['aarch64-linux-gnu-as'], 'arm64', ['--target=aarch64-linux-gnu']),
}


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def elfsections(obj):
    """Sekce objektu ELF z vlastního čtení hlaviček (nezávisle na objcopy a
    architektuře): {jméno: (typ, příznaky, velikost, zarovnání, obsah)}."""
    d = obj.read_bytes()
    assert d[:4] == b'\x7fELF'
    elf64 = d[4] == 2
    if elf64:
        shoff, = struct.unpack_from('<Q', d, 40)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 58)
    else:
        shoff, = struct.unpack_from('<I', d, 32)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 46)
    heads = []
    for i in range(shnum):
        o = shoff + i * shentsize
        if elf64:
            name, typ, flags, addr, off, size, link, info, align, ent = struct.unpack_from('<IIQQQQIIQQ', d, o)
        else:
            name, typ, flags, addr, off, size, link, info, align, ent = struct.unpack_from('<IIIIIIIIII', d, o)
        heads.append((name, typ, flags, off, size, align))
    strtab = heads[shstrndx][3]
    secs = {}
    for name, typ, flags, off, size, align in heads:
        n = d[strtab + name:d.index(b'\0', strtab + name)].decode()
        if typ in (0, 2, 3, 4, 9):
            continue
        data = b'' if typ == 8 else d[off:off + size]
        secs[n] = (typ, flags, size, align, data)
    return secs


def relocs(obj):
    out = run(['readelf', '-r', '-W', obj]).stdout
    res = {}
    cur = None
    for line in out.splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            cur = m.group(1)
            res.setdefault(cur, set())
            continue
        m = re.match(r'\s*([0-9a-f]+)\s+[0-9a-f]+\s+(\S+)\s+[0-9a-f]+\s+(\S+)(?:\s*([-+])\s*([0-9a-f]+))?', line)
        if m and cur:
            off, typ, sym, sign, add = m.groups()
            a = int(add, 16) if add else 0
            if sign == '-':
                a = -a
            res[cur].add((int(off, 16), typ, sym, a))
    return res


def symbols(obj):
    out = run(['readelf', '-s', '-W', obj]).stdout
    res = set()
    secnames = {}
    for m in re.finditer(r'\[\s*(\d+)\]\s+(\S+)\s', run(['readelf', '-S', '-W', obj]).stdout):
        secnames[m.group(1)] = m.group(2)
    for line in out.splitlines():
        m = re.match(r'\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+(\S+)\s+(\S*)', line)
        if not m:
            continue
        val, size, typ, bind, ndx, name = m.groups()
        if typ in ('SECTION', 'FILE') or not name or name in ('$x', '$d'):
            continue  # mapovací symboly arm64 se nezapisují
        res.add((name, int(val, 16), int(size), typ, bind, secnames.get(ndx, ndx)))
    return res


def compare(name, ref, new, tmp):
    bad = []
    rs, ns = elfsections(ref), elfsections(new)
    for s in sorted(set(rs) | set(ns)):
        if s not in rs or s not in ns:
            bad.append(f'section {s} only in {"gas" if s in rs else "luxir"}')
            continue
        if rs[s][:2] != ns[s][:2]:
            bad.append(f'section {s} type/flags {rs[s][:2]} != {ns[s][:2]}')
        if rs[s][3] != ns[s][3]:
            bad.append(f'section {s} alignment {rs[s][3]} != {ns[s][3]}')
        if rs[s][2] != ns[s][2]:
            bad.append(f'section {s} size {rs[s][2]} != {ns[s][2]}')
            continue
        a, b = rs[s][4], ns[s][4]
        if a != b:
            n = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            bad.append(f'section {s} differs at offset {n:#x} (gas {a[n:n+12].hex()} luxir {b[n:n+12].hex()})')
    rr, nr = relocs(ref), relocs(new)
    for s in sorted(set(rr) | set(nr)):
        a, b = rr.get(s, set()), nr.get(s, set())
        if a != b:
            only = sorted(a - b)[:3], sorted(b - a)[:3]
            bad.append(f'relocs {s}: gas-only {only[0]} luxir-only {only[1]}')
    a, b = symbols(ref), symbols(new)
    if a != b:
        bad.append(f'symbols: gas-only {sorted(a - b)[:3]} luxir-only {sorted(b - a)[:3]}')
    return bad


def ror(v, r, size):
    mask = (1 << size) - 1
    v &= mask
    return ((v >> r) | (v << (size - r))) & mask


def probes_arm64(tmp):
    """Všechny logické immediate (velikost prvku, počet jedniček, rotace)
    v mov/and/orr: kódování N:immr:imms musí sedět s GNU as."""
    lines = ['.text']
    for size in (2, 4, 8, 16, 32, 64):
        for ones in range(1, size):
            for rot in range(size):
                e = ror((1 << ones) - 1, rot, size)
                v = 0
                for k in range(0, 64, size):
                    v |= e << k
                lines.append(f'\tmov x0, #{v:#x}')
                lines.append(f'\tand x1, x2, #{v:#x}')
                if size <= 32:
                    lines.append(f'\tmov w0, #{v & 0xffffffff:#x}')
                    lines.append(f'\torr w1, w2, #{v & 0xffffffff:#x}')
    path = tmp / 'probe_bitmask.s'
    path.write_text('\n'.join(lines) + '\n')
    return [str(path)]


def probes(tmp):
    """Rodina sond relaxace skoků: k dlouhých jcc před sondou (kladný
    nárůst průchodu), skok přes zarovnání na cíl v různé vzdálenosti."""
    out = []
    for k in (1, 5, 10, 40):
        for al in (3, 4, 5):
            for n in range(100, 136, 3) if k < 40 else range(0, 136, 5):
                text = '\n'.join(['.text', 'f:', f'.rept {k}', 'jz far', '.endr', 'jmp L',
                                  f'.rept {n}', 'nop', '.endr', f'.p2align {al}', 'L: ret',
                                  '.rept 300', 'nop', '.endr', 'far: ret', ''])
                path = tmp / f'probe_{k}_{al}_{n}.s'
                path.write_text(text)
                out.append(str(path))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', action='append', required=True)
    ap.add_argument('--arch', default='x86_64', choices=sorted(ARCH))
    ap.add_argument('--lir', action='store_true')
    ap.add_argument('--probes', action='store_true', help='přidá generované sondy relaxace')
    ap.add_argument('-t', dest='target')
    ap.add_argument('inputs', nargs='*')
    a = ap.parse_args()
    gas, target, _ = ARCH[a.arch]
    target = a.target or target
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        inputs = list(a.inputs)
        if a.probes:
            inputs += probes_arm64(tmp) if a.arch == 'arm64' else probes(tmp)
        for backend in a.backend:
            backend = str(Path(backend).resolve())
            for inp in inputs:
                src = Path(inp)
                if a.lir:
                    s = tmp / (src.stem + '.s')
                    r = run([backend, '-t', target, '-o', s, src])
                    if r.returncode:
                        if 'not supported' in r.stderr or 'unsupported' in r.stderr:
                            print(f'skip {src.name}: {r.stderr.strip()[:120]}')
                            continue
                        print(f'FAIL {src.name}: backend: {r.stderr[:300]}')
                        failed += 1
                        continue
                    src = s
                text = src.read_text(errors='replace')
                if '@tpoff' in text or '@gotpcrel' in text or ':tprel_' in text or ':got' in text:
                    print(f'skip {inp}: thread-local or GOT references are not supported')
                    continue
                ref = tmp / 'ref.o'
                new = tmp / 'new.o'
                r = run(gas + ['-o', ref, src])
                if r.returncode:
                    print(f'skip {inp}: gas: {r.stderr.strip()[:200]}')
                    continue
                r = run([backend, '-a', '-t', target, '-o', new, src])
                if r.returncode:
                    print(f'FAIL {inp}: luxir -a: {r.stderr.strip()[:300]}')
                    failed += 1
                    continue
                bad = compare(inp, ref, new, tmp)
                if bad:
                    failed += 1
                    print(f'FAIL {inp} ({Path(backend).parent.name}/{Path(backend).name})')
                    for b in bad[:6]:
                        print('   ', b)
                else:
                    print(f'ok   {inp}')
    print(f'as_diff: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
