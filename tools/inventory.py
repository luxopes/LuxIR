#!/usr/bin/env python3
"""Parse every production LuxIR C translation unit; fail on unknown syntax.

This is developer tooling only. Generated LSL never depends on Python, C,
pycparser, or the reference QBE executable at runtime.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import argparse
from collections import Counter
from pycparser import c_parser, c_ast

ROOT = Path(__file__).resolve().parents[1]
# C reference sources of the backend (QBE 1.3 fork with the LuxIR reader).
UPSTREAM = ROOT / 'src'
INCLUDE = Path(__file__).resolve().parent / 'include'
VERSION = '1.3'
ARCHIVE_SHA256 = 'd587905d620dc5e1d2bfa7c2cc642b9b837aa89a3188c6e37b53d756cf66e320'
UPSTREAM_COMMIT = 'c0818978acec60ebb6167fade60fb7012cbf20ca'


def sources():
    """Use upstream's build manifest, not a hand-picked subset of passes."""
    result = subprocess.run(['make', '-s', 'src'], cwd=UPSTREAM,
                            capture_output=True, text=True, check=True)
    return [UPSTREAM / name for name in result.stdout.split()]


def parse(path):
    result = subprocess.run(['cc', '-E', '-std=c99', '-nostdinc',
                             '-I', str(INCLUDE), '-D__attribute__(x)=',
                             str(path)], cwd=UPSTREAM, capture_output=True,
                            text=True, check=True)
    # Preserve #line directives: pycparser coordinates link generated code to C.
    # pycparser 3.0 needs an extra (semantically neutral) pair of
    # parentheses around an array compound literal used as a postfix base.
    source = re.sub(r'(\([A-Za-z_][^;\n{}]*\[[^\n{}]*\]\)\{[^{}]+\})(?=\[)',
                    r'(\1)', result.stdout)
    # C labels have their own namespace (e.g. parse.c's `Ins:`), but
    # pycparser lexes typedef names before label parsing. Rename only labels.
    for label in set(re.findall(r'\bgoto\s+(\w+)\s*;', source)):
        source = re.sub(r'\bgoto\s+'+re.escape(label)+r'\s*;',
                        'goto qbe_label_'+label+';', source)
        source = re.sub(r'(?m)^(\s*)'+re.escape(label)+r'\s*:',
                        r'\1qbe_label_'+label+':', source)
    return c_parser.CParser().parse(source, filename=str(path))


class Inventory(c_ast.NodeVisitor):
    def __init__(self, source):
        self.source = str(source)
        self.functions = []
        self.nodes = Counter()
        self.calls = set()

    def visit(self, node):
        self.nodes[type(node).__name__] += 1
        return super().visit(node)

    def visit_FuncDef(self, node):
        self.functions.append({'name': node.decl.name, 'line': node.coord.line,
                               'origin': node.coord.file,
                               'static': 'static' in node.decl.storage})
        self.generic_visit(node)

    def visit_FuncCall(self, node):
        if isinstance(node.name, c_ast.ID):
            self.calls.add(node.name.name)
        self.generic_visit(node)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT/'build/inventory.json')
    args = parser.parse_args()
    report = {'version': VERSION, 'archive_sha256': ARCHIVE_SHA256,
              'upstream_commit': UPSTREAM_COMMIT, 'translation_units': []}
    for path in sources():
        tree = parse(path)
        inv = Inventory(path)
        inv.visit(tree)
        local = [f for f in inv.functions if Path(f['origin']) == path]
        report['translation_units'].append({
            'path': str(path.relative_to(UPSTREAM)),
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'functions': local, 'inline_helpers': len(inv.functions)-len(local),
            'node_counts': dict(sorted(inv.nodes.items())),
            'calls': sorted(inv.calls)})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(f"QBE {VERSION}: {len(report['translation_units'])} production units, "
          f"{sum(len(t['functions']) for t in report['translation_units'])} functions parsed")
    print(args.output)


if __name__ == '__main__':
    main()
