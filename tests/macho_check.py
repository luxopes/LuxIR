#!/usr/bin/env python3
"""Vestavěný linker pro macOS (luxir -l -t amd64_apple|arm64_apple) bez
macOS: program se slinkuje s mapou rozvržení (-M) a nezávisle na kódu
linkeru se ověří, že
  - bajty živých atomů jsou ve výstupu beze změny mimo pole relokací,
  - každá relokace ukazuje na správný cíl (instrukce a data dekódované
    z výstupu proti adresám symbolů podle mapy, importy na svůj stub),
  - absolutní ukazatele v datech mají rebase, ukazatele na importy bind,
  - stub skáče přes svou položku __got, která má bind na své jméno,
  - LC_MAIN ukazuje na _main, segmenty a podpis pokrývají soubor
    a hashe stránek v CodeDirectory odpovídají obsahu.

Použití: macho_check.py --backend LUXIR [--backend …] [--arch x86_64|arm64]
         [--program objekt.o … --tbd libSystem.tbd]
Bez --program se slinkují tests/run/*.lir s malým runtime v assembleru.
"""
import argparse, hashlib, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ARCH = {'x86_64': ('amd64_apple', 0x01000007), 'arm64': ('arm64_apple', 0x0100000c)}

# Minimální runtime pro tests/run: printf/puts přes write z libSystem.
RT = {
    'x86_64': '''
	.text
	.globl _main
_main:
	pushq %rbp
	movq %rsp, %rbp
	callq _lux_main
	movl %eax, %edi
	callq __exit
	.globl _printf
_printf:
	jmp _vprintf_stub
	.globl _puts
_puts:
	pushq %rbp
	movq %rsp, %rbp
	movq %rdi, %rsi
	movl $1, %edi
	movl $0, %edx
	leaq Lnl(%rip), %rax
	popq %rbp
	jmp _write
_vprintf_stub:
	movq %rdi, %rsi
	movl $1, %edi
	movl $4, %edx
	leaq _table(%rip), %rax
	cmpl $5, _table+8(%rip)
	jmp _write
	.section __TEXT,__cstring,cstring_literals
Lnl:	.asciz "\\n"
	.data
	.p2align 3
_table:	.quad _main
	.quad Lnl
	.quad _write
	.quad _printf+8
	.subsections_via_symbols
''',
    'arm64': '''
	.text
	.globl _main
_main:
	stp x29, x30, [sp, #-16]!
	bl _lux_main
	bl __exit
	.globl _printf
_printf:
	b _vprintf_stub
	.globl _puts
_puts:
	mov x1, x0
	mov x0, #1
	adrp x2, Lnl@PAGE
	add x2, x2, Lnl@PAGEOFF
	b _write
_vprintf_stub:
	mov x1, x0
	mov x0, #1
	mov x2, #4
	adrp x3, _table@PAGE+16
	ldr x3, [x3, _table@PAGEOFF+16]
	b _write
	.section __TEXT,__cstring,cstring_literals
Lnl:	.asciz "\\n"
	.data
	.p2align 3
_table:	.quad _main
	.quad Lnl
	.quad _write
	.quad _printf+8
	.subsections_via_symbols
''',
}
TBD = '''--- !tapi-tbd
tbd-version:     4
targets:         [ x86_64-macos, arm64-macos ]
install-name:    '/usr/lib/libSystem.B.dylib'
current-version: 1351
exports:
  - targets:         [ x86_64-macos, arm64-macos ]
    symbols:         [ __exit, _write ]
...
'''


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


class Obj:
    def __init__(self, path):
        d = self.d = Path(path).read_bytes()
        magic, self.cpu, _, ft, ncmds, _, self.flags, _ = struct.unpack_from('<IiiIIIII', d, 0)
        assert magic == 0xfeedfacf and ft == 1, path
        self.secs, self.syms = [], []
        o = 32
        for _ in range(ncmds):
            cmd, csz = struct.unpack_from('<II', d, o)
            if cmd == 0x19:
                ns, = struct.unpack_from('<I', d, o + 64)
                for k in range(ns):
                    s = o + 72 + 80 * k
                    name = d[s + 16:s + 32].rstrip(b'\0').decode() + ',' + d[s:s + 16].rstrip(b'\0').decode()
                    addr, size, off, al, rel, nrel, fl = struct.unpack_from('<QQIIIII', d, s + 32)
                    data = None if fl & 0xff == 1 else d[off:off + size]
                    rels, pend = [], None
                    for j in range(nrel):
                        a, info = struct.unpack_from('<iI', d, rel + 8 * j)
                        typ = info >> 28
                        if self.cpu == 0x0100000c and typ == 10:
                            v = info & 0xffffff
                            pend = v - (1 << 24) if v & 0x800000 else v
                            continue
                        rels.append({'off': a, 'num': info & 0xffffff, 'pcrel': (info >> 24) & 1, 'len': (info >> 25) & 3,
                                     'ext': (info >> 27) & 1, 'type': typ, 'addend': pend or 0})
                        pend = None
                    self.secs.append({'name': name, 'addr': addr, 'size': size, 'data': data, 'rels': rels, 'flags': fl})
            elif cmd == 2:
                symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', d, o + 8)
                st = d[stroff:stroff + strsize]
                for k in range(nsyms):
                    nx, ty, sc, _, val = struct.unpack_from('<IBBHQ', d, symoff + 16 * k)
                    self.syms.append({'name': st[nx:st.index(b'\0', nx)].decode(), 'type': ty, 'sect': sc, 'value': val})
            o += csz


class Exe:
    def __init__(self, path):
        d = self.d = Path(path).read_bytes()
        magic, self.cpu, _, ft, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', d, 0)
        assert magic == 0xfeedfacf and ft == 2, 'MH_EXECUTE'
        assert flags & 0x200085 == 0x200085, 'NOUNDEFS|DYLDLINK|TWOLEVEL|PIE'
        self.segs, self.secs, self.cmds = {}, {}, {}
        o = 32
        for _ in range(ncmds):
            cmd, csz = struct.unpack_from('<II', d, o)
            self.cmds.setdefault(cmd, []).append(o)
            if cmd == 0x19:
                name = d[o + 8:o + 24].rstrip(b'\0').decode()
                va, vs, fo, fs, mp, ip, ns, _ = struct.unpack_from('<QQQQiiII', d, o + 24)
                self.segs[name] = (va, vs, fo, fs, ip)
                for k in range(ns):
                    s = o + 72 + 80 * k
                    sn = d[s + 16:s + 32].rstrip(b'\0').decode() + ',' + d[s:s + 16].rstrip(b'\0').decode()
                    self.secs[sn] = struct.unpack_from('<QQIIIIIII', d, s + 32)
            o += csz
        assert o == 32 + sizeofcmds, 'load command sizes'

    def off(self, va):
        for va0, vs, fo, fs, _ in self.segs.values():
            if va0 <= va < va0 + fs:
                return fo + va - va0
        raise AssertionError(f'address {va:#x} not in a file-backed segment')

    def rd(self, va, n):
        o = self.off(va)
        return int.from_bytes(self.d[o:o + n], 'little')


def objdump(args, path):
    import shutil
    tools = ([shutil.which('llvm-objdump')] if shutil.which('llvm-objdump') else []) + \
        sorted(str(p) for p in Path('/usr/lib').glob('llvm-*/bin/llvm-objdump'))[::-1]
    for b in tools:
        r = run([b] + args + [path])
        if r.returncode == 0:
            return r.stdout
    raise AssertionError('llvm-objdump not found or failed')


def check(exe_path, objs, mapfile, arm):
    e = Exe(exe_path)
    atoms, imps = [], {}
    for line in Path(mapfile).read_text().splitlines():
        f = line.split()
        if f[0] == 'A':
            atoms.append(tuple(int(x) for x in f[1:]))
        else:
            imps[f[1]] = (int(f[2]), int(f[3]))
    byatom = {}
    for oi, si, start, end, out in atoms:
        byatom.setdefault((oi, si), []).append((start, end, out))
    for v in byatom.values():
        v.sort()

    def mapaddr(oi, si, a):
        cand = [t for t in byatom.get((oi, si), []) if t[0] <= a]
        assert cand, f'object {oi} section {si} address {a:#x} not in a live atom'
        start, end, out = cand[-1]
        return out + a - start

    defs = {}
    for oi, o in enumerate(objs):
        for s in o.syms:
            if s['type'] & 0xe == 0xe and s['type'] & 1:
                defs[s['name']] = (oi, s)

    def symaddr(oi, num):
        s = objs[oi].syms[num]
        if s['type'] & 1 and s['type'] & 0xe == 0:
            if s['name'] in defs:
                oi, s = defs[s['name']]
            else:
                assert s['name'] in imps, f'undefined {s["name"]}'
                return imps[s['name']][0], True
        return mapaddr(oi, s['sect'] - 1, s['value']), False

    import re
    rebase = set()
    for l in objdump(['--macho', '--rebase'], exe_path).splitlines():
        m = re.match(r'\s*\S+\s+\S+\s+0x([0-9A-Fa-f]+)\s', l)
        if m:
            rebase.add(int(m.group(1), 16))
    bind = {}
    for l in objdump(['--macho', '--bind'], exe_path).splitlines():
        m = re.match(r'\s*\S+\s+\S+\s+0x([0-9A-Fa-f]+)\s+pointer\s+(-?\d+)\s+\S+\s+(\S+)', l)
        if m:
            bind[int(m.group(1), 16)] = (m.group(3), int(m.group(2)))
    nrel = 0
    for oi, o in enumerate(objs):
        for si, s in enumerate(o.secs):
            for start, end, out in byatom.get((oi, si), []):
                if s['data'] is None:
                    continue
                masked = set()
                for r in s['rels']:
                    a = s['addr'] + r['off']
                    if start <= a < end:
                        masked.update(range(a, a + (1 << r['len'])))
                got = e.d[e.off(out):e.off(out) + end - start]
                for i in range(end - start):
                    if start + i not in masked:
                        assert got[i] == s['data'][start - s['addr'] + i], f'{s["name"]}: byte {start + i:#x} changed'
            for r in s['rels']:
                Pobj = s['addr'] + r['off']
                if not any(st <= Pobj < en for st, en, _ in byatom.get((oi, si), [])):
                    continue
                nrel += 1
                P = mapaddr(oi, si, Pobj)
                n = 1 << r['len']
                field = int.from_bytes(s['data'][r['off']:r['off'] + n], 'little', signed=True)
                if r['ext']:
                    T, imp = symaddr(oi, r['num'])
                else:
                    k = {6: 1, 7: 2, 8: 4}.get(r['type'], 0)
                    tobj = Pobj + 4 + k + field if r['pcrel'] else field
                    T, imp = mapaddr(oi, r['num'] - 1, tobj), False
                    field = (tobj - field - Pobj - 4 - k) if r['pcrel'] else 0
                    # na sekci: addend je posun cíle (žádný)
                if not arm:
                    if r['type'] == 0:
                        assert n == 8
                        if imp:
                            assert bind.get(P) == (objs[oi].syms[r['num']]['name'], field), f'bind at {P:#x}'
                        else:
                            assert e.rd(P, 8) == T + (field if r['ext'] else 0), f'UNSIGNED at {P:#x}'
                            assert P in rebase, f'rebase at {P:#x}'
                    else:
                        k = {6: 1, 7: 2, 8: 4}.get(r['type'], 0)
                        disp = int.from_bytes(e.d[e.off(P):e.off(P) + 4], 'little', signed=True)
                        if r['ext']:
                            want = T + field + k - (P + 4 + k)
                        else:
                            want = T - (P + 4 + k)
                        assert disp == want, f'{s["name"]}+{r["off"]:#x}: disp {disp:#x} != {want:#x}'
                    continue
                A = r['addend']
                ins = e.rd(P, 4)
                if r['type'] == 0:
                    assert n == 8
                    if imp:
                        assert bind.get(P) == (objs[oi].syms[r['num']]['name'], field), f'bind at {P:#x}'
                    else:
                        assert e.rd(P, 8) == T + field, f'UNSIGNED at {P:#x}'
                        assert P in rebase, f'rebase at {P:#x}'
                elif r['type'] == 2:
                    imm = ins & 0x3ffffff
                    imm = imm - (1 << 26) if imm & (1 << 25) else imm
                    assert P + 4 * imm == T + A, f'BRANCH26 at {P:#x}'
                elif r['type'] == 3:
                    imm = ((ins >> 29) & 3) | (((ins >> 5) & 0x7ffff) << 2)
                    imm = imm - (1 << 21) if imm & (1 << 20) else imm
                    assert (P & ~0xfff) + (imm << 12) == (T + A) & ~0xfff, f'PAGE21 at {P:#x}'
                elif r['type'] == 4:
                    imm = (ins >> 10) & 0xfff
                    sc = 0
                    if ins & 0x3b000000 == 0x39000000:
                        sc = ins >> 30
                        if ins & 0x04800000 == 0x04800000:
                            sc = 4
                    assert (imm << sc) == (T + A) & 0xfff, f'PAGEOFF12 at {P:#x}'
                else:
                    raise AssertionError(f'relocation type {r["type"]}')
    # stuby a GOT
    for name, (stub, got) in imps.items():
        if arm:
            i0, i1, i2 = e.rd(stub, 4), e.rd(stub + 4, 4), e.rd(stub + 8, 4)
            imm = ((i0 >> 29) & 3) | (((i0 >> 5) & 0x7ffff) << 2)
            imm = imm - (1 << 21) if imm & (1 << 20) else imm
            page = (stub & ~0xfff) + (imm << 12)
            assert page + (((i1 >> 10) & 0xfff) << 3) == got and i2 == 0xd61f0200 and i0 & 0x9f00001f == 0x90000010, f'stub {name}'
        else:
            assert e.d[e.off(stub):e.off(stub) + 2] == b'\xff\x25', f'stub {name}'
            disp = int.from_bytes(e.d[e.off(stub) + 2:e.off(stub) + 6], 'little', signed=True)
            assert stub + 6 + disp == got, f'stub {name}'
        assert bind.get(got) == (name, 0), f'GOT bind {name}'
    # LC_MAIN
    o = e.cmds[0x80000028][0]
    entry, = struct.unpack_from('<Q', e.d, o + 8)
    assert entry + 0x100000000 == symaddr(*next((oi, k) for oi, ob in enumerate(objs) for k, s in enumerate(ob.syms)
                                               if s['name'] == '_main' and s['type'] & 0xe == 0xe))[0], 'LC_MAIN'
    # podpis
    o = e.cmds[0x1d][0]
    sigoff, sigsize = struct.unpack_from('<II', e.d, o + 8)
    link = e.segs['__LINKEDIT']
    assert link[2] + link[3] == len(e.d) == sigoff + sigsize, 'signature at the end of __LINKEDIT'
    sb = e.d[sigoff:]
    assert struct.unpack_from('>III', sb, 0)[0] == 0xfade0cc0
    cdo, = struct.unpack_from('>I', sb, 16)
    cd = sb[cdo:]
    magic, _, ver, flags, hoff, ioff, nspec, nslots, limit = struct.unpack_from('>IIIIIIIII', cd, 0)
    assert magic == 0xfade0c02 and ver == 0x20400 and flags == 0x20002 and limit == sigoff and cd[37] == 2 and cd[39] == 12
    base, lim, fl = struct.unpack_from('>QQQ', cd, 64)
    assert (base, lim, fl) == (0, e.segs['__TEXT'][3], 1), 'exec segment'
    for k in range(nslots):
        page = e.d[4096 * k:min(4096 * (k + 1), limit)]
        assert cd[hoff + 32 * k:hoff + 32 * k + 32] == hashlib.sha256(page).digest(), f'page hash {k}'
    return nrel


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', action='append', required=True)
    ap.add_argument('--arch', action='append', choices=sorted(ARCH))
    ap.add_argument('--program', nargs='*')
    ap.add_argument('--tbd')
    a = ap.parse_args()
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'libSystem.tbd').write_text(TBD)
        for arch in a.arch or sorted(ARCH):
            target, _ = ARCH[arch]
            for backend in a.backend:
                backend = str(Path(backend).resolve())
                cases = []
                if a.program:
                    cases.append(('program', [Path(p) for p in a.program], Path(a.tbd)))
                else:
                    rt = tmp / 'rt.s'
                    rt.write_text(RT[arch])
                    r = run([backend, '-a', '-t', target, '-o', tmp / 'rt.o', rt])
                    if r.returncode:
                        print(f'FAIL    {arch} runtime: {r.stderr.strip()[:300]}')
                        failed += 1
                        continue
                    for lir in sorted((ROOT / 'run').glob('*.lir')):
                        if lir.stem == 'tls':
                            continue
                        s = tmp / 'p.s'
                        r = run([backend, '-t', target, '-o', s, lir])
                        if r.returncode:
                            print(f'skip    {arch}-{lir.stem}: {r.stderr.strip()[:120]}')
                            continue
                        # main programu jako _lux_main, aby runtime měl _main
                        text = s.read_text().replace('_main', '_lux_main') + '\n.subsections_via_symbols\n'
                        s.write_text(text)
                        r = run([backend, '-a', '-t', target, '-o', tmp / f'{lir.stem}.o', s])
                        if r.returncode:
                            print(f'FAIL    {arch}-{lir.stem}: {r.stderr.strip()[:300]}')
                            failed += 1
                            continue
                        cases.append((lir.stem, [tmp / f'{lir.stem}.o', tmp / 'rt.o'], tmp / 'libSystem.tbd'))
                for name, inputs, tbd in cases:
                    (tmp / 'again').mkdir(exist_ok=True)
                    exe, exe2, mp = tmp / 'p', tmp / 'again' / 'p', tmp / 'p.map'
                    r = run([backend, '-l', '-M', mp, '-t', target, '-o', exe] + inputs + [tbd])
                    if r.returncode == 0:
                        r = run([backend, '-l', '-t', target, '-o', exe2] + inputs + [tbd])
                    if r.returncode:
                        print(f'FAIL    {arch}-{name}: {r.stderr.strip()[:400]}')
                        failed += 1
                        continue
                    try:
                        assert exe.read_bytes() == exe2.read_bytes(), 'output not deterministic'
                        n = check(exe, [Obj(p) for p in inputs], mp, arch == 'arm64')
                        print(f'ok      {arch}-{name} ({n} relocations)')
                    except AssertionError as e:
                        print(f'FAIL    {arch}-{name}: {e}')
                        failed += 1
    print(f'macho_check: {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
