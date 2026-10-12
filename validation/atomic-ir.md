# SC atomic IR and Linux build driver — 12 October 2026

Six SC public operations: atomic_load, atomic_store, atomic_add,
atomic_xchg, atomic_cas and atomic_fence. Natural 4/8-byte alignment and
valid lifetimes are caller preconditions. No external atomic runtime calls.
Load forwarding stops at atomics; SC operations remain pinned, including
unused load/add/CAS results. C and native LSL backend assembly is identical.

1,202,047 checks per Linux target (amd64 native, arm64/rv64 QEMU): C11 oracle,
32/64-bit unsigned overflow and both CAS paths, 100,000 random patterns,
register pressure, globals, unused results, 4-thread add counters, 4-thread
CAS retries and 2,000 SC store-buffering rounds. Eight invalid IR forms
are rejected with identical diagnostics by both backends. All six target
assemblers accepted the atomic and numeric corpus. QEMU is not a formal
weak-memory proof or real-hardware performance measurement.

Final compiler: 0.1.2-dev, IR 1, runtime ABI none. Generated LSL is reproducible,
465/465 functions, zero blocked. make check and make check-runtime passed.
Self-host stage1 == stage2 == stage3: 55.5884 and 51.0280 seconds; both inside
60-second gate. See atomic-selfhost.json and current-backends.json.

The Python build driver uses GNU as/ld to produce a raw Linux static PIE
from IR main(argc,argv,envp). It automatically links raw runtime, installs
main TLS, applies relative RELA data/TLS pointers and protects GNU_RELRO.
Thirty build/execution checks per target, C/LSL binary equality, align 1..64
with tiny initialized/zero-fill TLS, actual RELRO write fault, dead function
section elimination and preservation of existing output on compilation error.
The relocation C oracle and LIR module passed 23 additional checks per target.

A new test exposed and fixed the variant-II TLS span using promoted TCB
alignment instead of the original PT_TLS alignment. Small TLS blocks now
match GNU linker TPOFF offsets. DCE applies to function sections; unused data
can still remain in shared sections. This is not a built-in assembler/linker,
managed ABI/GC/containers or Windows/macOS runtime implementation.
