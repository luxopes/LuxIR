# LuxIR 0.1.1

- Versioned IR 1 headers, explicit external runtime ABI `none`, compatible legacy input and matching C/LSL diagnostics.
- Merge the shared dominance, spill and bitset improvements from `perf/algorithms-after-0.1`.
- Experimental standalone luxrt modules: memory/UTF-8, Int64 conversion, checked arithmetic, Linux OS/syscall wrappers, startup/environment, full I/O and sequentially consistent atomics.
- Linux x86_64 raw threads (no TLS, libc callbacks or managed GC). Linux arm64/rv64 primitives and entry programs tested under qemu-user.
- rv64 opt-in `-p` PC-relative symbol addresses for static PIE. Absolute pointer data still require a startup relocator.
- Self-host fixed-point and explicit 60-second test, Linux matrix scripts and a C-reference Forgejo CI workflow.
- Controlled opera benchmark, raw measurements and explicit open performance TODOs. A loop-layout experiment without measured benefit was removed.

The shipped Linux x86_64 compiler is compiled from generated LSL and verified by self-host. GNU assembler/linker remain toolchain dependencies. Runtime modules are experimental external APIs; this release does not claim integrated managed ABI/GC, own assembler/linker, Float conversions, completed containers, or Windows/macOS/Android runtime. C/LSL output parity and Linux cross runtime checks passed; execution on native Windows/macOS and remote CI remain unverified.

New allocation findings already on remote main are preserved in TODO. Full validation history is in VALIDATION.md; release-specific build records and hashes are in verification.json and SHA256SUMS.
