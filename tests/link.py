#!/usr/bin/env python3
"""Režim -x: z .lir spustitelný program s runtime LSL (ABI lsl-0).

Ověřuje výstup a návratový kód příkladu, shodu C reference a LSL verze,
determinismus, vlastnosti obrazu (statický PIE bez loaderu a build-id,
nespustitelný zásobník), hledání runtime vedle luxir a chybové cesty
včetně úklidu dočasných souborů. Volitelně --lir porovná binárku s
referenční binárkou z LSL (--expect).
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / 'examples/lsl-runtime-hello.lir'
RUNTIME = ROOT / 'build/luxir-runtime'


def run(command, **kw):
    return subprocess.run([str(c) for c in command], capture_output=True, timeout=120, **kw)


def leftovers(directory):
    return sorted(p.name for p in Path(directory).iterdir() if p.suffix in ('.s', '.o'))


def image_checks(path):
    header = run(['readelf', '-h', '-l', '-S', '-n', path]).stdout.decode()
    assert 'DYN (Position-Independent Executable file)' in header, header
    assert 'INTERP' not in header and 'Requesting program interpreter' not in header
    assert '.note.gnu.build-id' not in header and 'Build ID' not in header
    stack = [line for line in header.splitlines() if 'GNU_STACK' in line]
    assert stack and 'RWE' not in header.split('GNU_STACK', 1)[1].split('\n', 2)[1], stack
    assert 'TEXTREL' not in run(['readelf', '-d', path]).stdout.decode()
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', type=Path, action='append', required=True)
    ap.add_argument('--runtime', type=Path, default=RUNTIME,
                    help='adresář s lsl-0/amd64_sysv.o (výchozí build/luxir-runtime)')
    ap.add_argument('--lir', type=Path, action='append', default=[],
                    help='.lir z LSL s hlavičkou runtime_abi "lsl-0"')
    ap.add_argument('--expect', type=Path, action='append', default=[],
                    help='binárka z LSL pro stejný --lir (stejné pořadí)')
    args = ap.parse_args()
    backends = [p.resolve() for p in args.backend]
    runtime = args.runtime.resolve()
    assert (runtime / 'lsl-0/amd64_sysv.o').is_file(), 'make runtime-lsl'
    checks = 0
    with tempfile.TemporaryDirectory(prefix='luxir-link-') as directory:
        work = Path(directory)
        images = []
        for backend in backends:
            out = work / ('hello-' + backend.parent.name)
            r = run([backend, '-x', '-L', runtime, '-o', out, EXAMPLE])
            assert r.returncode == 0 and not r.stdout and not r.stderr, r
            assert leftovers(work) == [], leftovers(work)
            p = run([out])
            assert (p.returncode, p.stdout, p.stderr) == (3, b'hello, lsl-0\n5050\n', b''), p
            checks += 2 + image_checks(out)
            # Determinismus: druhý překlad je bajtově stejný.
            again = work / 'again'
            assert run([backend, '-x', '-L', runtime, '-o', again, EXAMPLE]).returncode == 0
            assert again.read_bytes() == out.read_bytes()
            images.append(out.read_bytes())
            checks += 1
        assert all(image == images[0] for image in images), 'C/LSL executables differ'
        checks += 1

        # Výchozí hledání runtime: <dir luxir>/luxir-runtime, <dir>/../lib/luxir
        # a <dir>/../build/luxir-runtime (src/luxir ve vývojovém checkoutu).
        layouts = {'next': ('bin/luxir', 'bin/luxir-runtime'),
                   'prefix': ('bin/luxir', 'lib/luxir'),
                   'checkout': ('src/luxir', 'build/luxir-runtime')}
        for backend in backends:
            for layout, (program, directory) in layouts.items():
                tree = work / ('tree-' + layout)
                shutil.rmtree(tree, ignore_errors=True)
                binary = tree / program
                binary.parent.mkdir(parents=True)
                shutil.copy2(backend, binary)
                shutil.copytree(runtime, tree / directory)
                out = work / 'found'
                r = run([binary, '-x', '-o', out, EXAMPLE])
                assert r.returncode == 0 and not r.stderr, (layout, r)
                assert out.read_bytes() == images[0], layout
                checks += 1
            shutil.rmtree(work / 'tree-prefix')
            shutil.rmtree(work / 'tree-checkout')
            shutil.rmtree(work / 'tree-next' / 'bin/luxir-runtime')
            r = run([work / 'tree-next/bin/luxir', '-x', '-o', work / 'missing', EXAMPLE])
            assert r.returncode == 1 and b'runtime lsl-0 object is missing' in r.stderr, r
            assert not (work / 'missing').exists() and leftovers(work) == []
            checks += 1
            shutil.rmtree(work / 'tree-next')

        # Chybové cesty: hláška, kód 1, žádný výstup ani dočasné soubory.
        plain = work / 'plain.lir'
        plain.write_text('export function lsl_entry() -> Int64:\n    return 0\nend\n')
        unresolved = work / 'unresolved.lir'
        unresolved.write_text('runtime_abi "lsl-0"\nexport function lsl_entry() -> Int64:\n'
                              '    Int64(x) = call nowhere()\n    return x\nend\n')
        broken = work / 'broken.lir'
        broken.write_text('runtime_abi "lsl-0"\nexport function lsl_entry() -> Int64:\n    junk\nend\n')
        cases = [
            ('no-header', ['-x', '-L', runtime], plain, b'-x needs a runtime_abi "lsl-0" header'),
            ('target', ['-x', '-t', 'arm64', '-L', runtime], EXAMPLE, b'available only for amd64_sysv'),
            ('runtime-dir', ['-x', '-L', work / 'none'], EXAMPLE, b'runtime lsl-0 object is missing'),
            ('linker', ['-x', '-L', runtime], unresolved, b'linker failed (status 1)'),
            ('parse', ['-x', '-L', runtime], broken, b''),
        ]
        for backend in backends:
            for name, options, source, message in cases:
                out = work / 'failed'
                out.write_bytes(b'stale')
                r = run([backend, *options, '-o', out, source])
                assert r.returncode != 0 and message in r.stderr, (name, r)
                assert not out.exists(), (name, 'stale output kept')
                assert leftovers(work) == [], (name, leftovers(work))
                checks += 1
                for p in work.glob('failed*'):
                    p.unlink()
            r = run([backend, '-x', EXAMPLE])
            assert r.returncode == 1 and b'-x needs -o file' in r.stderr, r
            r = run([backend, '-x', '-d', 'P', '-o', work / 'dbg', EXAMPLE])
            assert r.returncode == 1 and b'-x needs -o file and no -d' in r.stderr, r
            checks += 2

        # Programy z LSL: stejná binárka jako dnešní sestavení v LSL.
        assert len(args.expect) in (0, len(args.lir))
        for index, lir in enumerate(args.lir):
            for backend in backends:
                out = work / ('lsl-' + str(index))
                r = run([backend, '-x', '-L', runtime, '-o', out, lir.resolve()])
                assert r.returncode == 0 and not r.stderr, (lir, r)
                if args.expect:
                    assert out.read_bytes() == args.expect[index].read_bytes(), lir
                checks += 1
    print(f'{checks} link checks passed ({len(backends)} backends)')


if __name__ == '__main__':
    main()
