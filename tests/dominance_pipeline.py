#!/usr/bin/env python3
"""Compare large random CFGs through the full old/new backend pipelines.

Back/forward/self edges exercise SSA construction, dominance frontiers, GVN
branch removal and subsequent dominance rebuilding. The chain makes every
block reachable. All targets must retain byte-identical assembly. Host tests
also execute the straight path and check its independently computed sum.
"""
from pathlib import Path
import argparse
import json
import random
import subprocess
import tempfile

TARGETS = ['amd64_sysv', 'amd64_apple', 'amd64_win', 'arm64', 'arm64_apple', 'rv64']


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, required=True)
    ap.add_argument('--backend', type=Path, required=True)
    ap.add_argument('--cases', type=int, default=36)
    ap.add_argument('--target', action='append', choices=TARGETS)
    ap.add_argument('--report', type=Path)
    args = ap.parse_args()
    rng = random.Random(0xD011A70)
    targets = args.target or TARGETS
    checks = executions = 0
    with tempfile.TemporaryDirectory(prefix='luxir-cfg-') as directory:
        directory = Path(directory)
        for case in range(args.cases):
            count = [2, 15, 63, 64, 65, 129, 257, 513, 1025][case % 9]
            lines = ['export function cfg(Int32(c)) -> Int64:',
                     '    Int64(v) = 0', '    goto b0']
            for index in range(count):
                lines.extend([f'b{index}:', f'    Int64(v) = add(v, {37 + index})'])
                if index + 1 == count:
                    lines.append('    return v')
                elif case % 3 == 0:
                    # Unconditional stretches between branches stress the
                    # conditional-ancestor index as well as its invalidation.
                    if index % 5:
                        lines.append(f'    goto b{index + 1}')
                    else:
                        successor = rng.randrange(count)
                        lines.extend([f'    Int32(test{index}) = eq(c, {index})',
                                      f'    if test{index} goto b{successor} else b{index + 1}'])
                else:
                    successor = rng.randrange(count)
                    lines.extend([f'    Int32(test{index}) = eq(c, {index})',
                                  f'    if test{index} goto b{successor} else b{index + 1}'])
            lines.append('end')
            source = directory / 'case.lir'
            source.write_text('\n'.join(lines) + '\n')
            for target in targets:
                outputs = []
                for backend in (args.baseline, args.backend):
                    result = subprocess.run([str(backend.resolve()), '-t', target, str(source)],
                                            capture_output=True, timeout=120, check=True)
                    outputs.append(result.stdout)
                assert outputs[0] == outputs[1], (case, count, target)
                checks += 1
                if target == 'amd64_sysv':
                    assembly = directory / 'case.s'
                    assembly.write_bytes(outputs[1])
                    driver = directory / 'driver.c'
                    expected = count * 37 + count * (count - 1) // 2
                    driver.write_text('#include <stdint.h>\nextern int64_t cfg(int32_t);\n'
                                      f'int main(void) {{ return cfg(-1) != {expected}; }}\n')
                    binary = directory / 'case'
                    subprocess.run(['cc', str(driver), str(assembly), '-o', str(binary)],
                                   capture_output=True, check=True)
                    subprocess.run([str(binary)], check=True, timeout=10)
                    executions += 1
    print(f'CFG pipeline: {checks} assembly comparisons, {executions} executions PASS')
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps({'cases': args.cases, 'targets': targets,
                                          'assembly_comparisons': checks,
                                          'executions': executions, 'complete': True}, indent=2) + '\n')


if __name__ == '__main__':
    main()
