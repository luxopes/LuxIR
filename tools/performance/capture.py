#!/usr/bin/env python3
"""Create a backend wrapper that saves lslc's IR then runs the real backend.

Paths are embedded in the wrapper because LSL's subprocess launcher filters
environment variables. Pass the generated wrapper as lslc's backend argument.
"""
import argparse
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output', type=Path, required=True)
ap.add_argument('--ir', type=Path, required=True)
ap.add_argument('--backend', type=Path, required=True)
args = ap.parse_args()
destination = args.ir.resolve()
backend = args.backend.resolve()
wrapper = args.output.resolve()
wrapper.parent.mkdir(parents=True, exist_ok=True)
destination.parent.mkdir(parents=True, exist_ok=True)
wrapper.write_text('#!/usr/bin/env python3\nimport os, shutil, sys\n'
                   f'shutil.copyfile(sys.argv[-1], {str(destination)!r})\n'
                   f'os.execv({str(backend)!r}, [{str(backend)!r}] + sys.argv[1:])\n')
wrapper.chmod(0o755)
print(wrapper)
