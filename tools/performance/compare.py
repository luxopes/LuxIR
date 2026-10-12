#!/usr/bin/env python3
"""Serial, alternating backend comparisons on identical input bytes.

Verify assembly equality before timing, warm both binaries, then measure
output to /dev/null. Preserve all samples and input/binary hashes. No frontend,
assembler or linker is included in these backend-only timings.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import resource
import statistics
import subprocess
import tempfile
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, required=True)
    ap.add_argument('--candidate', type=Path, required=True)
    ap.add_argument('--input', type=Path, action='append', required=True)
    ap.add_argument('--repeats', type=int, default=7)
    ap.add_argument('--cpu', type=int, default=2)
    ap.add_argument('--target', default='amd64_sysv')
    ap.add_argument('--report', type=Path, required=True)
    args = ap.parse_args()
    if args.repeats < 1:
        ap.error('--repeats must be positive')
    backends = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    report = {'platform': platform.platform(), 'target': args.target,
              'cpu_affinity': [args.cpu], 'repeats': args.repeats,
              'binaries': {k: {'path': str(v), 'sha256': digest(v)} for k, v in backends.items()},
              'cases': [], 'complete': False}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    def save():
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    def run(variant, source, output):
        command = [str(backends[variant]), '-t', args.target, '-o', str(output), str(source)]
        def setup():
            os.sched_setaffinity(0, {args.cpu})
            resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3, 4 * 1024**3))
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        started = time.monotonic()
        p = subprocess.run(command, capture_output=True, preexec_fn=setup, timeout=180)
        seconds = time.monotonic() - started
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        if p.returncode:
            raise RuntimeError(f'{command}: {p.returncode}: {p.stderr[-2000:]!r}')
        return {'variant': variant, 'seconds': seconds,
                'cpu_seconds': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
                'returncode': p.returncode}
    with tempfile.TemporaryDirectory(prefix='luxir-algorithms-') as directory:
        directory = Path(directory)
        for source in args.input:
            source = source.resolve()
            row = {'input': str(source), 'input_bytes': source.stat().st_size,
                   'input_sha256': digest(source), 'samples': []}
            report['cases'].append(row)
            for variant in backends:
                output = directory / (variant + '.s')
                run(variant, source, output)
            assert (directory / 'baseline.s').read_bytes() == (directory / 'candidate.s').read_bytes(), source
            row['assembly_matches'] = True
            row['assembly_sha256'] = digest(directory / 'baseline.s')
            for repeat in range(args.repeats):
                order = tuple(backends) if repeat % 2 == 0 else tuple(reversed(backends))
                for variant in order:
                    sample = run(variant, source, '/dev/null')
                    sample['repeat'] = repeat
                    row['samples'].append(sample)
                    save()
            for metric in ('seconds', 'cpu_seconds'):
                row['median_' + metric] = {v: statistics.median(s[metric] for s in row['samples'] if s['variant'] == v) for v in backends}
                row['ratio_' + metric] = row['median_' + metric]['candidate'] / row['median_' + metric]['baseline']
            print(source.name, row['median_seconds'], row['ratio_seconds'], flush=True)
            save()
    report['complete'] = True
    save()


if __name__ == '__main__':
    main()
