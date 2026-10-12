#!/bin/sh
# Complete locally exercised Linux matrix including an existing native LSL backend.
# Requires QBE 1.3, cross GCC/libc, qemu-user, clang and GNU binutils on PATH.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
make check QBE="${QBE:-qbe}"
make check-runtime
for target in arm64 rv64; do
    python3 tests/numeric.py --target "$target" --backend src/luxir --backend build/luxir
    python3 tests/runtime/run_tls.py --target "$target" --backend src/luxir --backend build/luxir
    python3 tests/runtime/tls_entry.py --target "$target" --backend src/luxir --backend build/luxir
    python3 tests/runtime/run_platform.py --target "$target" --backend src/luxir --backend build/luxir
    python3 tests/run_tests.py --target "$target" --backend src/luxir --backend build/luxir
    python3 tests/runtime/cross_linux.py --target "$target" --backend src/luxir --backend build/luxir
    python3 tests/runtime/entry.py --target "$target" --backend src/luxir --backend build/luxir
done
python3 tests/rv64_pie.py --backend src/luxir --backend build/luxir
tests/assemble.sh build/luxir tests/run/*.lir examples/*.lir
# Deliberately explicit: cross execution is not cross self-host or platform performance.
