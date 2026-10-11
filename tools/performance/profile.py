#!/usr/bin/env python3
"""Generate an isolated phase-instrumented LSL backend, leaving production intact.

Linux clock_gettime(CLOCK_THREAD_CPUTIME_ID) measures CPU nanoseconds. Timers
surround top-level pass calls, plus inclusive parser and function callbacks.
Only the instrumented executable has profiling code. Use -o /dev/null so its
stdout can carry the report without changing the generated assembly.
"""
from pathlib import Path
import argparse
import json
import re
import shutil

ROOT = Path(__file__).resolve().parents[2]
FUNCTION = re.compile(r'^function (\w+)\([^\n]*\):\n.*?^end\n', re.M | re.S)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT / 'lsl')
    ap.add_argument('--output', type=Path, default=ROOT / 'build/performance/profile')
    args = ap.parse_args()
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    for module in args.source.glob('*.lsl'):
        shutil.copyfile(module, directory / module.name)
    source = (directory / 'luxir.lsl').read_text()
    functions = {m[1]: m[0] for m in FUNCTION.finditer(source)}
    names = []
    def index(name):
        if name not in names:
            names.append(name)
        return names.index(name)

    body = functions['q_f_main_func']
    pattern = re.compile(r'^(    )(q_v\d+) = (q_f_\w+|__lsl_qbe_call[12])\((.*)\)$', re.M)
    indirect = {'q_v23': 'abi0', 'q_v112': 'abi1', 'q_v125': 'isel', 'q_v192': 'emitfn'}
    seen_indirect = set()
    def timed(match):
        if match[3].startswith('__lsl_qbe_call'):
            if match[2] not in indirect:
                raise RuntimeError('main_func changed: audit indirect phase mappings')
            seen_indirect.add(match[2])
        name = indirect.get(match[2], match[3])
        slot = index(name)
        timer = 'perf_start_' + match[2]
        return f'    {timer} = __lsl_qbe_raw_int(prof.now())\n' + match[0] + f'\n    call prof.finish({slot}, {timer})'
    instrumented = pattern.sub(timed, body)
    if seen_indirect != set(indirect):
        raise RuntimeError('main_func changed: expected four indirect phases')
    source = source.replace(body, instrumented)
    for name, phase in [('q_f_main_func', 'function_inclusive'), ('q_f_parse', 'parse_inclusive')]:
        body = next(m[0] for m in FUNCTION.finditer(source) if m[1] == name)
        lines = body.splitlines(True)
        lines.insert(1, '    perf_function_start = __lsl_qbe_raw_int(prof.now())\n')
        body2 = ''.join(lines)
        slot = index(phase)
        body2 = body2.replace('    return q_result\n', f'    call prof.finish({slot}, perf_function_start)\n    return q_result\n')
        source = source.replace(body, body2)
    source = source.replace('load luxir_memory as mem', 'load luxir_profile as prof\nload luxir_memory as mem', 1)
    # QBE's main terminates through exit(0), so a report after its call would
    # be unreachable. Insert immediately before the final successful exit.
    main_body = next(m[0] for m in FUNCTION.finditer(source) if m[1] == 'q_f_main')
    exits = list(re.finditer(r'^    q_v\d+ = libc.c_exit\(0\)$', main_body, re.M))
    assert exits
    position = exits[-1].start()
    source = source.replace(main_body, main_body[:position] + '    call prof.report()\n' + main_body[position:])
    (directory / 'luxir.lsl').write_text(source)
    runtime = '''# lsl: internal-abi
_counts = __lsl_int_zeros(128)
_data = __lsl_qbe_load64(__lsl_qbe_raw_int(_counts) + 24)
function now():
    time = __lsl_qbe_alloc(16)
    if __lsl_syscall6(228, 3, time, 0, 0, 0, 0) != 0:
        Error(OSError: "clock_gettime failed")
    end
    return __lsl_qbe_load64(time) * 1000000000 + __lsl_qbe_load64(time + 8)
end
function finish(raw_index, raw_start):
    index = __lsl_qbe_raw_int(raw_index)
    started = __lsl_qbe_raw_int(raw_start)
    elapsed = __lsl_qbe_raw_int(now()) - started
    address = _data + index * 16
    call __lsl_qbe_store64(address, __lsl_qbe_load64(address) + elapsed)
    call __lsl_qbe_store64(address + 8, __lsl_qbe_load64(address + 8) + 1)
    return 0
end
function report():
'''
    for slot, name in enumerate(names):
        runtime += f'    print "{name} " + String(__lsl_qbe_load64(_data + {slot*16})) + " " + String(__lsl_qbe_load64(_data + {slot*16+8}))\n'
    runtime += '    return 0\nend\n'
    (directory / 'luxir_profile.lsl').write_text(runtime)
    (directory / 'phases.json').write_text(json.dumps(names, indent=2) + '\n')
    print('Prepared', len(names), 'phase counters in', directory)


if __name__ == '__main__':
    main()
