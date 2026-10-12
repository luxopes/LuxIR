# Portable numeric IR — 12 October 2026

Six public operations: sqrt, umulh, mulh, clz, ctz and popcount.
C reference and generated native LSL backend passed 1,110,145 independent
checks per Linux target (amd64 native, arm64/rv64 QEMU), plus invalid-form
rejection. High product uses signed/unsigned 128-bit oracle; counts use
independent bit loops. Tests cover zero, signed/unsigned boundaries, every
bit position, 100,000 random pairs, register pressure, same-operand multiply,
Float subnormals, infinities, NaNs, signed zero and four rounding modes.
All six assembler targets accepted the new instruction corpus.

Legacy matrix: protocol 108 checks, dominance 6,435,072 checks, bitsets 12,384,
run programs + 36 invalid forms per backend, QBE differential 456 pairs per
backend, and 300 fuzz cases: passed. Generated source: 464/464 functions,
0 blocked. Self-host stage1 == stage2 == stage3, 53.9693 and 50.0918 seconds,
both below the 60-second gate in this environment.

Isolated native umulh benchmark: 1 million pairs, 16 repeats per trial,
7 alternating-order trials, allocation/input preparation outside timing.
Every result checked against uint128 oracle. Median new loop 0.771751 ms,
four-32-bit-multiply IR fallback 2.284390 ms: 2.9600x faster. Both loops
compiled by the same native backend, same load/control/reduction structure.
This is not a whole LSL application or Float conversion benchmark.

No Windows/macOS runtime execution claim; their new code was assembled only.
See JSON reports and tests/numeric.py, numeric_benchmark.py for reproduction.
