# Luxrt ABI 1: Linux64 explicit roots

ABI 1 is independent of LSL. Any frontend can emit ordinary LuxIR calls
using the signatures in `runtime/abi-v1.json`. Supported targets are Linux
`amd64_sysv`, `arm64`, and `rv64`, little-endian, 64-bit pointers. The OS
portion currently uses documented raw Linux flags and buffer layouts;
this profile does not provide Win32/libSystem implementations.

## Selection and compatibility

Put `runtime_abi "1"` before definitions. The backend emits a retained
8-byte relocation in `.data.rel.ro.luxir.abi` to
`luxrt_abi_v1_le64_explicit`. GNU ELF section flag `R` prevents garbage
collection of this requirement, including empty/data-only units. A linker
must resolve the guard; a different revision is an error. The guard is a
compatibility mechanism, not authentication of untrusted runtime code.
Tooling requires GNU binutils with SHF_GNU_RETAIN (2.36+) or equivalent.

The guard contains four UInt64 words: magic `0x31545258554c`, ABI revision
1, scalar/pointer width 64, endian indicator 1 (little). An ABI-breaking
change to layouts, signatures, calling/error/ownership semantics requires
a new revision and new guard symbol. Adding independent functions is
additive; existing ABI 1 definitions must retain their semantics.

`tools/luxir_build.py` automatically includes the ABI and managed modules
for inputs declaring 1; `--managed` is unnecessary. It builds static PIE,
initializes main TLS and startup relocations, and protects RELRO. It does
not create a managed heap or root application globals without IR calls.
`none` and omitted headers remain external-runtime mode: no guard and no
certification of external symbols. Header state resets per input file.

## Calls, errors, and memory

Functions use the ordinary native scalar C calling convention. `Ptr` is
a valid address; UInt64/Int64/Value/raw Float64 bits use IR `Int64`, status
UInt32/Int32 uses `Int32`. Callback addresses are `Ptr`. No public aggregate
by-value arguments or returns, varargs, or hidden language state registers.

Managed constructors and mutations return Int32 status and write results
to explicit output pointers on success. Status 0 succeeds; typical errors
are -22 type/argument/syntax, -34 range/overflow, -12 OOM, -84 invalid UTF-8,
-2 missing key/end iteration, -16 active root frames at heap destruction.
Individual raw APIs document count returns and additional negative codes.
Linux OS calls return nonnegative results or negative Linux errno; no
ambient libc errno. There are no implicit exceptions or TLS error flags.
Failure preserves documented outputs/logical contents, though allocation
failure may follow a completed GC. Error propagation can use an IR `invoke` status call with explicit success
and failure successors. There is no automatic unwinding/cleanup or native
exception landing-pad protocol.

Callers provide valid appropriately aligned buffers, contexts and output
slots with the documented lifetimes. This is not validation of arbitrary
addresses or forged Value words. Use size queries for context storage:
`luxrt_heap_bytes` (336), `luxrt_roots_bytes` (24),
`luxrt_descriptor_bytes` (48), aligned to at least 8 bytes. C headers expose
these layouts for C consumers; their ABI 1 sizes are frozen. Do not access
private heap/object/collection fields from a frontend; use public APIs.

## Values and descriptors

Value is UInt64. Low-three-bit tag 1 encodes signed 61-bit Int (`i << 3 | 1`);
2 is None, 3 False, 11 True. Nonzero tag 0 is a live managed-object address.
Zero is an unused root slot, not a valid collection item or None. Full
Int64 outside the immediate range boxes; Float64 boxes preserve all IEEE
bits. Raw pointers must not be placed in Value roots.

Managed type IDs: Int 1, None 2, Bool 3, wide Int implementation object 16
(reported as Int), Float 17, String 18, Values list 19, Map 20, Set 21,
IntArray 22, FloatArray 23, ByteArray 24, Tuple 25. IDs below 256 are
reserved, including private backing types. Frontend object types use IDs
256 and above; IDs alone are not structural type equivalence.

A 48-byte descriptor contains six 8-byte fields in order: type_id,
reference_count, reference_offsets pointer, repeat_offset, count_offset,
stride. Offsets describe Value fields relative to object payload.
Stride 0 disables repeated slots; otherwise count is UInt64 at count_offset
and Value fields are at repeat_offset + i*stride. Descriptor/maps remain
valid and immutable for the lifetime of their objects. `object_new` zeros
payload. Use `object_payload` to obtain its address. Objects do not move.

## GC and ownership

Initialize a caller-owned heap with max mapped bytes and page size.
Each heap has one owner; no concurrent calls or shared-heap managed values.
Heap APIs are not async-signal-safe: no allocation/collection from signal
handlers or reentrant heap operations.
Multiple independent heaps are allowed. A collection follows explicit
root frames and descriptor Value maps. There is no conservative scanning
of machine words or global object registry. Optional compiler `Value`/
`gc_heap` metadata generates precise shadow frames; native PC stack maps
are not implemented.

A 24-byte root frame is `{previous Ptr, slots Ptr, count UInt64}`. Push
live Values with `roots_push` before any allocation-capable call, and pop
in LIFO order, or annotate managed locals with compiler `Value`/`gc_heap`
so those function-local frames are generated automatically. Initialize unused slots to 0. Slots and frames must remain
valid and immobile until popped. Root all other live Values, not merely
arguments: a call may trigger GC. Calls documented to root their own
arguments do not protect unrelated locals/globals. A borrowed span into a
managed payload requires the containing object to remain rooted too.

Immediates need no heap allocation. Objects referenced by roots or other
live objects survive. References to foreign heaps are rejected. Collection
is non-moving iterative mark-sweep; released blocks are reused through
size-class freelists. Mapped slabs are held until `heap_destroy`, which
requires all root frames popped. Malformed maps stop tracing without sweep.
There are no finalizers, nursery, compaction or stop-the-world protocol.
Raw thread callbacks must obey their separate constraints and cannot use
a shared managed heap. These capabilities must be added in later work.

## Collections and numeric services

String is immutable strict UTF-8, has cached hash and stores its byte and
codepoint lengths; embedded NUL is allowed. Numeric adapters are strict,
locale-free and do not implement implicit language-specific coercion.
Float formatting/decimal parsing uses nearest/ties-even; text roundtrip
preserves finite bits and signed zero, not NaN payload identity.

Values lists and unboxed arrays are mutable; Tuple references are immutable
but may point to mutable objects. Array indices are unsigned. A frontend
implements its language's negative-index and slice normalization. Map/Set
preserve insertion order and choose type-distinct or numeric-key equality
via flags. Map iteration forbids mutation. Default hashes are deterministic and do not
provide collision-flooding defense. `luxrt_heap_seed_hash` opts into keyed
SipHash-2-4 for cached String and scalar Map/Set keys; use an unpredictable secret
128-bit key. It returns -16 while any objects are alive, and may allocate the
first slab (-12 on failure). `luxrt_heap_init_secure` initializes a fresh heap with all 16 entropy bytes from
OS getrandom, retrying partial reads/EINTR and failing closed without changing
the output heap on error. `luxrt_heap_seed_hash` supports explicitly supplied
keys; callers choosing that API must supply unpredictability themselves. Seed
is private slab metadata, propagated on growth; the ABI heap size stays 336.
Destroy clears the seed. A String's reported cached hash is heap-specific, so
read-only foreign-heap lookup recomputes it in the target heap; insertion retains
the existing same-heap ownership requirement. Hashes are not persistent IDs. Tuple structural equality/hash is not supplied.

Each header specifies detailed statuses, capacity/bounds, output preservation
and borrowing. `runtime/abi-v1.json` is generated by
`tools/runtime_manifest.py`; `--check` detects stale declarations. The ABI
integration test links every advertised function on each declared target,
checks missing/wrong revision rejection and runs raw standalone fixtures.
