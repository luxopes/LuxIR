#!/usr/bin/env python3
"""Protocol headers: compatibility, diagnostics and C/LSL parity."""
import argparse
import subprocess
import tempfile
from pathlib import Path

TARGETS = ['amd64_sysv', 'amd64_win', 'amd64_apple', 'arm64', 'arm64_apple', 'rv64']
BODY = 'export function answer() -> Int32:\n    return 42\nend\n'
HEAD = 'ir_version "1"\nruntime_abi "none"\n'
HEAD_LSL = 'ir_version "1"\nruntime_abi "lsl-0"\n'
ERRORS = [
    ('ir', 'ir_version "2"\n', 'unsupported IR version "2"'),
    ('ir-zero', 'ir_version "0"\n', 'unsupported IR version "0"'),
    ('ir-overflow', 'ir_version "18446744073709551617"\n', 'unsupported IR version'),
    ('abi', 'runtime_abi "1"\n', 'unsupported runtime ABI "1"'),
    ('abi-lsl', 'runtime_abi "lsl-1"\n', 'unsupported runtime ABI "lsl-1"; supported: none, lsl-0'),
    ('duplicate-mixed', 'runtime_abi "none"\nruntime_abi "lsl-0"\n', 'duplicate runtime_abi'),
    ('duplicate-ir', 'ir_version "1"\nir_version "1"\n', 'duplicate ir_version'),
    ('duplicate-abi', 'runtime_abi "none"\nruntime_abi "none"\n', 'duplicate runtime_abi'),
    ('late-ir', BODY + 'ir_version "1"\n', 'version headers must precede definitions'),
    ('late-abi', 'data x = Int32(0)\nruntime_abi "none"\n', 'version headers must precede definitions'),
    ('integer', 'ir_version 1\n', 'string expected'),
    ('missing', 'ir_version\n', 'string expected'),
    ('trailing', 'ir_version "1" junk\n', 'newline expected after version header'),
    ('linkage', 'export ir_version "1"\n', 'only data and function have linkage'),
]

def run(backend, arguments):
    return subprocess.run([str(backend), *map(str, arguments)], capture_output=True, timeout=30)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backend', type=Path, action='append', required=True)
    args = ap.parse_args()
    backends = [p.resolve() for p in args.backend]
    checks = 0
    with tempfile.TemporaryDirectory(prefix='luxir-version-') as directory:
        source = Path(directory) / 'input.lir'
        for backend in backends:
            r = run(backend, ['--version'])
            assert (r.returncode, r.stdout, r.stderr) == (0, b'LuxIR 0.3.0 (IR 1; runtime ABI none, lsl-0)\n', b''), r
            checks += 1
        # Headers must leave every target's code byte-identical to legacy IR.
        for target in TARGETS:
            reference = None
            for text in [BODY, HEAD + BODY, '# comment\n\n' + HEAD + BODY,
                         'runtime_abi "none"\nir_version "1"\n' + BODY,
                         'ir_version "1"\n' + BODY, 'runtime_abi "none"\n' + BODY,
                         HEAD_LSL + BODY, 'runtime_abi "lsl-0"\n' + BODY]:
                source.write_text(text)
                for backend in backends:
                    r = run(backend, ['-t', target, source])
                    assert r.returncode == 0 and not r.stderr, r.stderr
                    if reference is None:
                        reference = r.stdout
                    assert r.stdout == reference, (backend, target, text)
                    checks += 1
        source.write_text('')
        empty = run(backends[0], [source]).stdout
        for text in ['ir_version "1"', 'runtime_abi "none"', HEAD, 'runtime_abi "lsl-0"', HEAD_LSL]:
            source.write_text(text)
            for backend in backends:
                r = run(backend, [source])
                assert r.returncode == 0 and r.stdout == empty and r.stderr == b'', r
                checks += 1
        for name, text, message in ERRORS:
            source.write_text(text)
            reference = None
            for backend in backends:
                r = run(backend, [source])
                assert r.returncode != 0 and message.encode() in r.stderr, (name, r.stderr)
                assert r.stderr.startswith(b'luxir:'), (name, r)
                # Streaming output may already contain previous definitions;
                # a late header must still fail, with identical C/LSL output.
                if not name.startswith('late-'):
                    assert r.stdout == b'', (name, r)
                result = (r.returncode, r.stdout, r.stderr)
                if reference is None:
                    reference = result
                assert result == reference, (name, 'C/LSL diagnostic mismatch')
                checks += 1
        # Per-file state must reset, including stdin and multiple inputs.
        source.write_text(HEAD)
        for backend in backends:
            r = run(backend, [source, source])
            assert r.returncode == 0 and r.stdout == empty and not r.stderr, r
            r = subprocess.run([str(backend), '-'], input=(HEAD + BODY).encode(),
                               capture_output=True, timeout=30)
            assert r.returncode == 0 and not r.stderr, r
            checks += 2
        # runtime_abi platí pro celý program: soubory se nesmí rozcházet.
        other = Path(directory) / 'other.lir'
        other.write_text(HEAD_LSL)
        for backend in backends:
            r = run(backend, [source, other])
            assert r.returncode != 0 and b'conflicting runtime_abi headers' in r.stderr, r
            checks += 1
            # Soubor bez hlavičky runtime ABI nemění.
            source.write_text(BODY)
            r = run(backend, [other, source])
            assert r.returncode == 0 and not r.stderr, r
            source.write_text(HEAD)
            checks += 1
    print(f'{checks} protocol checks passed ({len(backends)} backends, 6 targets)')

if __name__ == '__main__':
    main()
