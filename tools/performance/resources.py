#!/usr/bin/env python3
"""Run one command and record wall/child CPU time and Linux peak RSS.

Fork a fresh worker so RUSAGE_CHILDREN does not inherit an invoking shell's
previous measurements across exec. With subprocesses, RSS is the largest child
peak, not the sum of a simultaneously running process tree.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import subprocess
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--report', type=Path, required=True)
    ap.add_argument('--cpu', type=int, default=2)
    ap.add_argument('command', nargs=argparse.REMAINDER)
    args = ap.parse_args()
    command = args.command
    if command and command[0] == '--':
        command = command[1:]
    if not command:
        ap.error('command required after --')
    started = time.monotonic()
    result = subprocess.run(command, preexec_fn=lambda: os.sched_setaffinity(0, {args.cpu}),
                            timeout=300)
    elapsed = time.monotonic() - started
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    report = {'command': command, 'returncode': result.returncode,
              'wall_seconds': elapsed, 'child_cpu_seconds': usage.ru_utime + usage.ru_stime,
              'peak_child_rss_kib': usage.ru_maxrss, 'cpu_affinity': [args.cpu]}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
    return result.returncode


if __name__ == '__main__':
    worker = os.fork()
    if worker == 0:
        raise SystemExit(main())
    _, status = os.waitpid(worker, 0)
    raise SystemExit(os.waitstatus_to_exitcode(status))
