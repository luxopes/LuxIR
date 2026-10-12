# Platform runtime acceptance — 12 October 2026

All three Linux targets passed C reference execution, runtime modules compiled
by the C backend, and the same modules compiled by the native LSL backend.
The two backends emitted identical assembler in TLS and signal/context tests;
the libc-free main TLS test also produced byte-identical static PIE binaries.

| Target | TLS checks | Signal checks | Context checks | Main TLS | Execution |
| --- | ---: | ---: | ---: | --- | --- |
| x86_64 | 513 | 16 | 8 | static PIE, no libc/loader | native |
| arm64 | 513 | 16 | 8 | static PIE, no libc/loader | QEMU |
| rv64 | 513 | 16 | 8 | static PIE, no libc/loader | QEMU |

TLS: 40 guarded-stack raw threads run an actual IR callback using
thread(symbol), each with initialized private data and aligned zero-fill.
The libc-hosted C oracle tests the same behavior with C local-exec TLS.
Invalid template alignment, missing image, overflow and invalid model
are rejected. The standalone entry test installs main-thread TLS itself,
checks initial data/zero-fill/write, current TP, rejection of freeing active
TLS, and kernel stack top. GNU as/ld are still used to build the binary.

Signals: invalid action leaves output unchanged; query returns the installed
handler; blocked targeted SIGUSR1 is delivered after unblock, another signal
returns normally, and previous mask/action are restored. The second execution
uses an IR-generated handler, not only the C oracle handler.

Context: bounds canaries and caller-SP slot, architecture word count, kernel
stack pointer, and a toy conservative marker for a real mapped object whose
root is only in a callee-saved register inside the assembly fixture. Allocation
metadata is encoded so it is not a pointer root. This is not a production GC,
not a complete stack-scanner integration, and not a stop-the-world test.

`make check-runtime` also passed the existing memory, UTF-8, integer, checked
arithmetic, OS, startup, I/O, atomics, legacy raw-thread and standalone suites.
Legacy threads: 40 threads, 4,000,000 increments. New services do not require
an LSL-specific object representation. Native LSL compiler was rebuilt from
committed generated source; compiler source/IR grammar have not changed.

Reproduce: `make check-runtime`; cross targets additionally run
`tests/runtime/{run_tls,tls_entry,run_platform}.py --target arm64|rv64
--backend src/luxir --backend build/luxir`. Cross GCC, GNU binutils and QEMU
must be on PATH. JSON execution reports are alongside this file.

No performance claims about real ARM/RV hardware, Windows/macOS support,
dynamic TLS, libc in raw callbacks or integrated GC follow from these tests.
