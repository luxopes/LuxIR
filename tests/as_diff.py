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
    # bez komprese a relaxace: tak kóduje vestavěný assembler
    'rv64': (['riscv64-linux-gnu-as', '-march=rv64g', '-mno-relax'], 'rv64', ['--target=riscv64-linux-gnu']),
    # COFF pro Windows (mingw-w64): objekt musí být shodný bajt po bajtu
    'x86_64-coff': (['x86_64-w64-mingw32-as'], 'amd64_win', ['--target=x86_64-w64-windows-gnu']),
    # arm64 pro Windows: vzorem je LLVM (clang), objekt shodný bajt po bajtu
    'arm64-coff': (['clang', '--target=aarch64-windows-gnu', '-c', '-x', 'assembler'], 'arm64_win', ['--target=aarch64-windows-gnu']),
    'arm': (['arm-linux-gnueabihf-as', '-march=armv7-a', '-mfpu=vfpv3-d16', '-mfloat-abi=hard'], 'arm', ['--target=armv7a-linux-gnueabihf']),
}
IGNORED_SECTIONS = {'.riscv.attributes', '.ARM.attributes'}   # vestavěný assembler atributy nezapisuje


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
        if typ in (0, 2, 3, 4, 9) or n in IGNORED_SECTIONS:
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
        if typ in ('SECTION', 'FILE') or not name or name.startswith('$'):
            continue  # mapovací symboly ($x, $d, $a …) se nezapisují
        res.add((name, int(val, 16), int(size), typ, bind, secnames.get(ndx, ndx)))
    return res


def coffdiff(ref, new):
    """COFF: první rozdíl a souhrn hlavičky, sekcí a počtu symbolů."""
    a, b = ref.read_bytes(), new.read_bytes()
    if a == b:
        return []
    n = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
    bad = [f'object differs at offset {n:#x} (gas {a[n:n+12].hex()} luxir {b[n:n+12].hex()}), sizes {len(a)} {len(b)}']
    for d, who in ((a, 'gas'), (b, 'luxir')):
        nsec, = struct.unpack_from('<H', d, 2)
        symptr, nsym = struct.unpack_from('<II', d, 8)
        heads = [(d[20 + 40 * i:28 + 40 * i].rstrip(b'\0').decode(errors='replace'),) + struct.unpack_from('<IIIHI', d, 36 + 40 * i)
                 for i in range(nsec)]
        bad.append(f'{who}: {nsec} sections, {nsym} symbols, ' + ', '.join(f'{h[0]} size {h[1]} relocs {h[4]} flags {h[5]:#x}' for h in heads[:6]))
    return bad


def compare(name, ref, new, tmp):
    if ref.read_bytes()[:4] != b'\x7fELF':
        return coffdiff(ref, new)
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


def probes_rv64(tmp):
    """RISC-V: konstanty li všech tvarů (rozklad load_const) a relaxace
    podmíněných větví kolem dosahu ±4 KiB, včetně zarovnání mezi větví
    a cílem, cílů za jinou větví a globálních cílů."""
    import random
    rnd = random.Random(7)
    vals = set()
    for k in range(64):
        for v in (1 << k, (1 << k) - 1, -(1 << k), ~(1 << k), (1 << k) + 1, (1 << k) - 0x800, (1 << k) + 0x800, (1 << k) + 0x7ff, (1 << k) - 0x7ff):
            vals.add(v)
    for _ in range(2000):
        w = rnd.choice([12, 13, 20, 31, 32, 33, 40, 48, 52, 63, 64])
        v = rnd.getrandbits(w)
        if rnd.random() < 0.5:
            v = -v
        vals.add(v)
        vals.add(v << rnd.randrange(0, 20))
    lines = ['.text']
    for v in sorted(vals):
        v = ((v + (1 << 63)) % (1 << 64)) - (1 << 63)
        lines.append(f'\tli a0, {v}')
    li = tmp / 'probe_li.s'
    li.write_text('\n'.join(lines) + '\n')
    out = [str(li)]
    lines = []
    k = 0
    for n in list(range(1018, 1027)) + [1, 500, 2000, 1021, 1022]:
        nops = '\n'.join(['\tnop'] * n)
        for al in (0, 3, 4):
            align = f'\t.p2align {al}\n' if al else ''
            k += 1
            lines.append(f'.section .text.p{k},"ax",@progbits\n\tbeq a0, a1, 1f\n{nops}\n{align}1:\n\tret')
            k += 1
            lines.append(f'.section .text.p{k},"ax",@progbits\n1:\n{nops}\n{align}\tbne a0, a1, 1b\n\tret')
            k += 1
            lines.append(f'.section .text.p{k},"ax",@progbits\n\tbeq a0, a1, 2f\n{nops}\n{align}\tblt a0, a1, 3f\n\tnop\n2:\n\tnop\n\tnop\n3:\n\tret')
            k += 1
            lines.append(f'.section .text.p{k},"ax",@progbits\n\tbltu a0, a1, g{k}\n{nops}\n{align}\t.globl g{k}\ng{k}:\n\tret')
            k += 1
            lines.append(f'.section .text.p{k},"ax",@progbits\n\tbgt a0, a1, 1f\n\tbge a0, a1, 1f\n{nops}\n{align}1:\n\tj 1b\n\tbeqz a0, 1b\n\tbnez a0, ext\n\tblez a0, 1f\n1:\n\tret')
    br = tmp / 'probe_branch.s'
    br.write_text('\n'.join(lines) + '\n')
    out.append(str(br))
    return out


def probes_arm(tmp):
    """ARM: konstanty datových instrukcí (rotované immediate, náhrady
    mov/mvn, and/bic, add/sub, adc/sbc, cmp/cmn, mov jako movw) a všech
    256 konstant VFP pro vmov."""
    import random
    rnd = random.Random(11)
    vals = set()
    for rot in range(0, 32, 2):
        for imm in (1, 0x7f, 0x80, 0xff, 0x55, 0xa5):
            v = ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff
            vals |= {v, (~v) & 0xffffffff, (-v) & 0xffffffff}
    for _ in range(1500):
        vals.add(rnd.getrandbits(rnd.choice([8, 12, 16, 20, 32])))
    def enc(v):
        v &= 0xffffffff
        return any(((v << i) | (v >> (32 - i))) & 0xffffffff <= 0xff for i in range(0, 32, 2))
    inv = lambda v: enc(~v)
    neg = lambda v: enc(-v)
    # které konstanty GNU as přijme: přímo, nebo přes náhradu instrukce
    ok = {'mov': lambda v: enc(v) or inv(v) or 0 <= v <= 0xffff, 'mvn': lambda v: enc(v) or inv(v),
          'and': lambda v: enc(v) or inv(v), 'bic': lambda v: enc(v) or inv(v),
          'adc': lambda v: enc(v) or inv(v), 'sbc': lambda v: enc(v) or inv(v),
          'add': lambda v: enc(v) or neg(v), 'sub': lambda v: enc(v) or neg(v),
          'cmp': lambda v: enc(v) or neg(v), 'cmn': lambda v: enc(v) or neg(v),
          'orr': enc, 'eor': enc, 'rsb': enc, 'tst': enc, 'teq': enc}
    lines = ['.syntax unified', '.arm', '.text']
    for v in sorted(vals):
        for op in ('mov', 'mvn', 'and', 'bic', 'add', 'sub', 'adc', 'sbc', 'orr', 'eor', 'rsb'):
            if ok[op](v):
                lines.append(f'\t{op} r0, r1, #{v}' if op not in ('mov', 'mvn') else f'\t{op} r0, #{v}')
        for op in ('cmp', 'cmn', 'tst', 'teq'):
            if ok[op](v):
                lines.append(f'\t{op} r0, #{v}')
        if ok['add'](v):
            lines.append(f'\tadds r0, r1, #{v}')
        if enc(v) or inv(v):
            lines.append(f'\tmovs r0, #{v}')
        if ok['mov'](v):
            lines.append(f'\tmoveq r0, #{v}')
    for imm8 in range(256):
        sign = -1.0 if imm8 & 0x80 else 1.0
        b = (imm8 >> 6) & 1
        cd = (imm8 >> 4) & 3
        efgh = imm8 & 0xf
        # exponent: NOT(b):b…b:cd, tedy 2^(cd+1) pro b = 0 a 2^(cd-3) pro b = 1
        exp = cd + 1 if b == 0 else cd - 3
        v = sign * (1.0 + efgh / 16.0) * 2.0 ** exp
        lines.append(f'\tvmov.f64 d0, #{v!r}')
        lines.append(f'\tvmov.f32 s0, #{v!r}')
    path = tmp / 'probe_arm.s'
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
    ap.add_argument('-p', action='store_true', help='backend s -p (adresy relativně k PC)')
    ap.add_argument('inputs', nargs='*')
    a = ap.parse_args()
    gas, target, _ = ARCH[a.arch]
    target = a.target or target
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        inputs = list(a.inputs)
        if a.probes:
            inputs += {'arm64': probes_arm64, 'arm64-coff': probes_arm64, 'rv64': probes_rv64, 'arm': probes_arm}.get(a.arch, probes)(tmp)
        for backend in a.backend:
            backend = str(Path(backend).resolve())
            for inp in inputs:
                src = Path(inp)
                if a.lir:
                    s = tmp / (src.stem + '.s')
                    r = run([backend, '-t', target] + (['-p'] if a.p else []) + ['-o', s, src])
                    if r.returncode:
                        if 'not supported' in r.stderr or 'unsupported' in r.stderr:
                            print(f'skip {src.name}: {r.stderr.strip()[:120]}')
                            continue
                        print(f'FAIL {src.name}: backend: {r.stderr[:300]}')
                        failed += 1
                        continue
                    src = s
                text = src.read_text(errors='replace')
                if '@tpoff' in text or '@gotpcrel' in text or ':tprel_' in text or ':got' in text or '%tprel' in text:
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
