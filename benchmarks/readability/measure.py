#!/usr/bin/env python3
"""Validate and measure raw CFG, structured raw memory and indexed IntArray.

No concurrent compilations/benchmarks. Both members of a comparison use
identical inputs/repetitions. Alternate ordering, keep individual samples,
check outputs against an independent Python calculation, and record CPU time
as well as wall time. Run build.py first.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import resource
import re
import shutil
import statistics
import subprocess
import sys
import time
from prepare import BUILD, HERE, ROOT

MASK = (1 << 64) - 1


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected(mode, count, repetitions):
    pattern = lambda i: ((i * 1234567) ^ (-6148914691236517206)) & MASK
    words = [pattern(i) for i in range(count)]
    checksum = 0
    if mode == 'count':
        checksum = sum(w.bit_count() for w in words) * repetitions
    elif mode in ('equal', 'unequal'):
        checksum = repetitions if mode == 'equal' or count == 0 else 0
    elif mode == 'bulk':
        words = [pattern(i+17) & (~pattern(i+91) & MASK) for i in range(count)]
    elif mode == 'membership':
        for repeat in range(repetitions):
            bit = repeat * 17 % (count * 64)
            words[bit // 64] |= 1 << (bit % 64)
            bit = (repeat * 31 + 1) % (count * 64)
            words[bit // 64] &= ~(1 << (bit % 64)) & MASK
    lines = [str(checksum)]
    if mode in ('bulk', 'membership'):
        lines += [str(w if w < 1 << 63 else w - (1 << 64)) for w in words]
    return ('\n'.join(lines) + '\n').encode()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--toolchain', type=Path, required=True)
    ap.add_argument('--reference', type=Path, required=True, help='QBE 1.3 C executable')
    ap.add_argument('--repeats', type=int, default=7)
    ap.add_argument('--cpu', type=int, default=2)
    ap.add_argument('--only-typed-array', action='store_true', help='Append the typed IntArray experiment to a completed report')
    args = ap.parse_args()
    if args.repeats < 1:
        ap.error('--repeats must be positive')
    tc = args.toolchain.resolve()
    report = {'platform': platform.platform(), 'cpu_affinity': [args.cpu],
              'repeats': args.repeats,
              'reference_sha256': digest(args.reference.resolve()),
              'compiler_sha256': digest(tc / 'bin/lslc'),
              'variants': json.loads((BUILD / 'variants.json').read_text()),
              'commands': [], 'micro': [], 'backend': [], 'complete': False}
    destination = BUILD / 'measurements.json'
    if args.only_typed_array:
        report = json.loads(destination.read_text())
        assert report['complete'], 'Finish the initial comparison first'
        assert report['compiler_sha256'] == digest(tc / 'bin/lslc'), 'Use the same compiler'
        assert not any(r['pair'] == ['raw', 'array_typed'] for r in report['micro']), 'Typed experiment already present'
        report['complete'] = False

    def save():
        destination.write_text(json.dumps(report, indent=2) + '\n')

    def run(command, timeout=180, record=False):
        command = list(map(str, command))
        def setup():
            os.sched_setaffinity(0, {args.cpu})
            resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3, 4 * 1024**3))
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        started = time.monotonic()
        p = subprocess.run(command, capture_output=True, timeout=timeout, preexec_fn=setup)
        seconds = time.monotonic() - started
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        row = {'command': command, 'seconds': seconds,
               'cpu_seconds': (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime),
               'returncode': p.returncode, 'stdout_sha256': hashlib.sha256(p.stdout).hexdigest()}
        if record:
            report['commands'].append(row)
            save()
        if p.returncode:
            raise RuntimeError(f'{command}: {p.returncode}\n{p.stdout[-2000:]!r}\n{p.stderr[-4000:]!r}')
        return row, p.stdout

    # Full backend language/diagnostic tests and all-target upstream differential.
    for variant in (() if args.only_typed_array else ('raw', 'readable')):
        backend = BUILD / variant / 'backend'
        report[variant+'_binary_sha256'] = digest(backend)
        row, out = run([sys.executable, ROOT / 'tests/run_tests.py', '--backend', backend], record=True)
        print(variant, out.decode().strip().splitlines()[-1], flush=True)
        row, out = run([sys.executable, ROOT / 'tests/differential.py', '--reference', args.reference.resolve(),
                        '--backend', backend, '--report', BUILD / variant / 'differential.json'], record=True)
        print(variant, out.decode().strip(), flush=True)

    # Add IntArray storage, keeping the bulk algorithm and four-way unrolling.
    array = BUILD / 'array'
    array.mkdir(exist_ok=True)
    array_source = (HERE / 'kernels_array.lsl').read_text()
    for name, op in [('bscopy', ''), ('bsunion', '|'), ('bsinter', '&'), ('bsdiff', '& ~')]:
        array_source += f'\nfunction q_f_{name}(destination, source):\n'
        array_source += '    count = len(destination)\n    if count != len(source):\n        Error(ValueError: "bitset size mismatch")\n    end\n    index = 0\n'
        def assignment(index):
            left, right = f'destination[{index}]', f'source[{index}]'
            value = right if not op else f'{left} {op} {right}'
            # LSL uses binary ~ for XOR, so complement is XOR with -1.
            if op == '& ~':
                value = f'{left} & (-1 ~ {right})'
            return f'{left} = {value}'
        array_source += '    while index + 4 <= count:\n'
        for offset in range(4):
            array_source += '        ' + assignment(f'index + {offset}') + '\n'
        array_source += '        index += 4\n    end\n    while index < count:\n'
        array_source += '        ' + assignment('index') + '\n        index += 1\n    end\n    return 0\nend\n'
    (array / 'kernels.lsl').write_text(array_source)
    typed = BUILD / 'array_typed'
    typed.mkdir(exist_ok=True)
    def type_reads(source):
        # Keep ordinary array indexing/type/bounds checks. Only make the
        # integer nature of the retrieved elements explicit to the compiler.
        pattern = re.compile(r'\b(words|left|right|source|destination)\[[^\]\n]+\]')
        def replace(match):
            rest = source[match.end():]
            if re.match(r'\s*=(?!=)', rest):  # assignment destination
                return match[0]
            return '__lsl_typed_int(' + match[0] + ')'
        return pattern.sub(replace, source)
    (typed / 'kernels.lsl').write_text(type_reads(array_source))
    variants = ('array_typed',) if args.only_typed_array else ('raw', 'readable', 'array', 'array_typed')
    for variant in variants:
        directory = BUILD / variant
        shutil.copyfile(HERE / 'micro.lsl', directory / 'micro.lsl')
        shutil.copyfile(HERE / ('storage_array.lsl' if variant.startswith('array') else 'storage_raw.lsl'), directory / 'storage.lsl')
        capture = directory / 'capture_micro.py'
        capture.write_text('#!/usr/bin/env python3\nimport shutil, subprocess, sys\n'
                           f'shutil.copyfile(sys.argv[-1], {str(directory / "micro.lir")!r})\n'
                           f'p = subprocess.run([{str(tc / "bin/luxir")!r}] + sys.argv[1:])\n'
                           f'if p.returncode == 0: shutil.copyfile(sys.argv[sys.argv.index("-o")+1], {str(directory / "micro.s")!r})\n'
                           'sys.exit(p.returncode)\n')
        capture.chmod(0o755)
        run([tc / 'bin/lslc', directory / 'micro.lsl', directory / 'micro', capture], record=True)
        (directory / 'micro').chmod(0o755)
        print('built micro', variant, flush=True)

    # Independent correctness checks, including empty sets, tails of unrolled
    # loops, signed/high bits, equality early returns, and mutations.
    checks = 0
    for variant in variants:
        for count in (0, 1, 2, 3, 4, 5, 7, 8, 9, 16, 31, 64, 257):
            for mode in ('count', 'bulk', 'equal', 'unequal', 'membership'):
                if mode == 'membership' and count == 0:
                    continue
                row, out = run([BUILD / variant / 'micro', mode, count, 11])
                assert out == expected(mode, count, 11), (variant, mode, count, out[:200])
                checks += 1
    report['independent_micro_checks_passed'] = report.get('independent_micro_checks_passed', 0) + checks
    print('independent micro checks', checks, 'PASS', flush=True)
    save()

    # Compare syntax separately from indexed storage. Calibrate each pair so
    # the slower member takes ~0.4 s, with identical repetitions for that pair.
    pairs = [('raw', 'array_typed')] if args.only_typed_array else [('raw', 'readable'), ('raw', 'array'), ('raw', 'array_typed')]
    for pair in pairs:
        for mode, count in [('count', 256), ('bulk', 256), ('equal', 256), ('membership', 256)]:
            pilot_count = 2000
            pilots = {}
            for variant in pair:
                row, out = run([BUILD / variant / 'micro', mode, count, pilot_count])
                assert out == expected(mode, count, pilot_count)
                pilots[variant] = row
            repetitions = max(2000, min(20000000, int(pilot_count * .4 / max(p['cpu_seconds'] for p in pilots.values()))))
            wanted = expected(mode, count, repetitions)
            row = {'pair': list(pair), 'mode': mode, 'words': count, 'repetitions': repetitions,
                   'pilots': pilots, 'samples': []}
            report['micro'].append(row)
            for repeat in range(args.repeats):
                for variant in (pair if repeat % 2 == 0 else pair[::-1]):
                    sample, out = run([BUILD / variant / 'micro', mode, count, repetitions], timeout=60)
                    assert out == wanted, (variant, mode, 'measured checksum')
                    sample.update(variant=variant, repeat=repeat, correct=True)
                    row['samples'].append(sample)
                    save()
            for metric in ('seconds', 'cpu_seconds'):
                row['median_'+metric] = {v: statistics.median(s[metric] for s in row['samples'] if s['variant'] == v) for v in pair}
                row['ratio_'+metric] = row['median_'+metric][pair[1]] / row['median_'+metric][pair[0]]
            print(pair, mode, row['median_seconds'], 'ratio', row['ratio_seconds'], flush=True)
            save()

    # Actual compiler frontend/backend IR, same bytes for both variants.
    inputs = [] if args.only_typed_array else [('frontend', BUILD / 'frontend.lir'), ('backend', BUILD / 'raw/backend.lir')]
    for name, source in inputs:
        row = {'input': name, 'bytes': source.stat().st_size, 'sha256': digest(source), 'samples': []}
        report['backend'].append(row)
        wanted = None
        for variant in ('raw', 'readable'):
            _, out = run([BUILD / variant / 'backend', source])  # warmup
            if wanted is None:
                wanted = hashlib.sha256(out).hexdigest()
            assert hashlib.sha256(out).hexdigest() == wanted
        for repeat in range(args.repeats):
            for variant in (('raw', 'readable') if repeat % 2 == 0 else ('readable', 'raw')):
                sample, out = run([BUILD / variant / 'backend', source])
                assert hashlib.sha256(out).hexdigest() == wanted, (name, variant)
                sample.update(variant=variant, repeat=repeat, assembly_matches=True)
                row['samples'].append(sample)
                save()
        row['assembly_sha256'] = wanted
        for metric in ('seconds', 'cpu_seconds'):
            row['median_'+metric] = {v: statistics.median(s[metric] for s in row['samples'] if s['variant'] == v) for v in ('raw', 'readable')}
            row['ratio_'+metric] = row['median_'+metric]['readable'] / row['median_'+metric]['raw']
        print('backend', name, row['median_seconds'], 'ratio', row['ratio_seconds'], flush=True)
        save()
    report['artifacts'] = {}
    for variant in ('raw', 'readable', 'array', 'array_typed'):
        report['artifacts'][variant] = {name: digest(BUILD / variant / name)
                                        for name in ('kernels.lsl', 'storage.lsl', 'micro.lsl', 'micro', 'micro.lir', 'micro.s')}
    report['differential_summary'] = {v: json.loads((BUILD / v / 'differential.json').read_text())['summary'] for v in ('raw', 'readable')}
    report['complete'] = True
    save()


if __name__ == '__main__':
    main()
