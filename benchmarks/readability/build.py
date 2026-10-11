#!/usr/bin/env python3
"""Build both native LSL backends with one fixed compiler/backend pair."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import time
from prepare import BUILD, prepare


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--toolchain', type=Path, required=True, help='Unpacked LSL 0.9.1 root')
    args = ap.parse_args()
    tc = args.toolchain.resolve()
    report = {'variants': prepare(), 'commands': [], 'complete': False,
              'compiler_sha256': hashlib.sha256((tc / 'bin/lslc').read_bytes()).hexdigest(),
              'bootstrap_backend_sha256': hashlib.sha256((tc / 'bin/luxir').read_bytes()).hexdigest()}
    destination = BUILD / 'build.json'

    def run(command):
        started = time.monotonic()
        p = subprocess.run(list(map(str, command)), capture_output=True, timeout=240)
        row = {'command': list(map(str, command)), 'seconds': time.monotonic()-started,
               'returncode': p.returncode, 'stdout': p.stdout.decode(errors='replace'),
               'stderr': p.stderr.decode(errors='replace')}
        report['commands'].append(row)
        destination.write_text(json.dumps(report, indent=2)+'\n')
        print(row, flush=True)
        if p.returncode:
            raise SystemExit(p.returncode)

    for variant in ('raw', 'readable'):
        directory = BUILD / variant
        capture = directory / 'capture_backend.py'
        # Capture actual compiler-generated input before scratch cleanup.
        capture.write_text('#!/usr/bin/env python3\nimport os, shutil, sys\n'
                           f'shutil.copyfile(sys.argv[-1], {str(directory / "backend.lir")!r})\n'
                           f'os.execv({str(tc / "bin/luxir")!r}, ["luxir"] + sys.argv[1:])\n')
        capture.chmod(0o755)
        run([tc / 'bin/lslc', directory / 'luxir.lsl', directory / 'backend', capture])
        (directory / 'backend').chmod(0o755)
        report[variant+'_binary_sha256'] = hashlib.sha256((directory / 'backend').read_bytes()).hexdigest()
    capture = BUILD / 'capture_frontend.py'
    capture.write_text('#!/usr/bin/env python3\nimport os, shutil, sys\n'
                       f'shutil.copyfile(sys.argv[-1], {str(BUILD / "frontend.lir")!r})\n'
                       f'os.execv({str(tc / "bin/luxir")!r}, ["luxir"] + sys.argv[1:])\n')
    capture.chmod(0o755)
    run([tc / 'bin/lslc', tc / 'lib/lsl/compiler/pure/lslc_qbe.lsl', BUILD / 'frontend', capture])
    report['complete'] = True
    destination.write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
