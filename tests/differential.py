#!/usr/bin/env python3
"""Diferenční test LuxIR backendu proti referenčnímu QBE 1.3.

Každý vstup QBE IL se převede nástrojem qbe2lir na LuxIR. Referenční C QBE
zpracuje původní IL, testovaný backend LuxIR. Pro každý cíl se porovná
návratový kód a výsledný assembler bajt po bajtu.
"""
import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import qbe2lir  # noqa: E402

TARGETS = ['amd64_sysv', 'amd64_apple', 'amd64_win', 'arm64', 'arm64_apple', 'rv64']


def run(cmd, timeout):
    started = time.monotonic()
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr, time.monotonic() - started
    except subprocess.TimeoutExpired:
        return None, b'', b'timeout', time.monotonic() - started


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reference', type=Path, required=True, help='C QBE 1.3 (qbe)')
    ap.add_argument('--backend', type=Path, required=True, help='LuxIR backend')
    ap.add_argument('--target', action='append', choices=TARGETS)
    ap.add_argument('--timeout', type=float, default=120.0)
    ap.add_argument('--report', type=Path)
    ap.add_argument('inputs', nargs='*', type=Path,
                    help='QBE IL files (default: tests/upstream/*.ssa)')
    args = ap.parse_args()
    inputs = args.inputs or sorted((ROOT / 'tests/upstream').glob('*.ssa'))
    targets = args.target or TARGETS
    report = {'reference': str(args.reference), 'backend': str(args.backend),
              'targets': targets, 'cases': [], 'summary': {}}
    failures = 0
    with tempfile.TemporaryDirectory(prefix='luxir-diff-') as tmp:
        tmp = Path(tmp)
        for source in inputs:
            lir = tmp / (source.stem + '.lir')
            lir.write_text(qbe2lir.convert(source.read_text()))
            for target in targets:
                rc_ref, out_ref, err_ref, t_ref = run(
                    [str(args.reference), '-t', target, str(source)], args.timeout)
                rc_lux, out_lux, err_lux, t_lux = run(
                    [str(args.backend), '-t', target, str(lir)], args.timeout)
                ok = (rc_ref == 0) == (rc_lux == 0) and (rc_ref != 0 or out_ref == out_lux)
                case = {'input': source.name, 'target': target, 'ok': ok,
                        'reference_status': rc_ref, 'backend_status': rc_lux,
                        'reference_seconds': round(t_ref, 4),
                        'backend_seconds': round(t_lux, 4)}
                summary = report['summary'].setdefault(target, {'cases': 0, 'failures': 0})
                summary['cases'] += 1
                if not ok:
                    failures += 1
                    summary['failures'] += 1
                    case['reference_stderr'] = err_ref.decode('utf-8', 'replace')[-2000:]
                    case['backend_stderr'] = err_lux.decode('utf-8', 'replace')[-2000:]
                    print(f'FAIL {source.name} {target}: status {rc_ref}/{rc_lux}'
                          f' {case["backend_stderr"].strip()[:200]}', flush=True)
                report['cases'].append(case)
    total = sum(s['cases'] for s in report['summary'].values())
    print(f'{total} input/target pairs, {failures} failures')
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
