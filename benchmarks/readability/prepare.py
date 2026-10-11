#!/usr/bin/env python3
"""Prepare isolated variants; never edit generated production LSL.

Readable replacements retain the memory layout, assertions and unroll factor.
"""
from pathlib import Path
import hashlib
import json
import re
import shutil

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / 'build/readability'
FUNCTION = re.compile(r'^function (\w+)\([^\n]*\):\n.*?^end\n', re.M | re.S)


def functions(source):
    return {m[1]: m[0] for m in FUNCTION.finditer(source)}


def prepare():
    original = (ROOT / 'lsl/luxir.lsl').read_text()
    expected = '64034e255699544ff1400b9c8bd22cb1e7c611af36474622843425d996c42410'
    if hashlib.sha256(original.encode()).hexdigest() != expected:
        raise RuntimeError('Baseline changed: audit and repin the readability experiment first')
    originals = functions(original)
    replacements = functions((HERE / 'readable.lsl').read_text())
    changed = original
    for name, body in replacements.items():
        assert name in originals
        changed = changed.replace(originals[name], body, 1)
    for variant, source in [('raw', original), ('readable', changed)]:
        directory = BUILD / variant
        directory.mkdir(parents=True, exist_ok=True)
        for module in (ROOT / 'lsl').glob('*.lsl'):
            if module.name != 'luxir.lsl':
                shutil.copyfile(module, directory / module.name)
        (directory / 'luxir.lsl').write_text(source)
    # Standalone hot-kernel modules, with identical assertions and backing store.
    # Assertions are active; their strings are copied from the production unit.
    for variant, source in [('raw', original), ('readable', changed)]:
        fs = functions(source)
        header = '# lsl: internal-abi\nload luxir_memory as mem\n'
        for name in ('q_string_31', 'q_string_48', 'q_string_49'):
            assignments = re.findall(r'^' + name + r' = .*$', original, re.M)
            assert len(assignments) == 1
            header += assignments[0] + '\n'
        header += '''function q_assert(condition, expression, file, line):
    if __lsl_qbe_raw_int(condition) == 0:
        Error(ValueError: "bitset assertion failed")
    end
    return 0
end
'''
        (BUILD / variant / 'kernels.lsl').write_text(header + '\n'.join(fs[n] for n in replacements))
    manifest = {
        'baseline_commit': '7480a0fe0a70ec9525cbae7363cf9da457194fce',
        'baseline_source_sha256': hashlib.sha256(original.encode()).hexdigest(),
        'modified_functions': sorted(replacements),
        'original_lines': sum(originals[n].count('\n') for n in replacements),
        'readable_lines': sum(s.count('\n') for s in replacements.values()),
        'readable_source_sha256': hashlib.sha256(changed.encode()).hexdigest(),
    }
    (BUILD / 'variants.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


if __name__ == '__main__':
    print(json.dumps(prepare(), indent=2))
