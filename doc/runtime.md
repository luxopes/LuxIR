# Kontrakt runtime LuxIR

Podporované kontrakty jsou `runtime_abi "none"` a `runtime_abi "1"`.
Legacy/no-header/none nemění assembler a neurčuje kompatibilitu externí
knihovny. ABI 1 je Linux64 little-endian profil, native scalar C calling
convention, Value model a přesný single-owner GC s explicitními roots.
Normativní kontrakt je v [abi-v1.md](abi-v1.md); strojově čitelný seznam
156 public funkcí je v [runtime/abi-v1.json](../runtime/abi-v1.json).

ABI 1 hlavička přidá retenovanou relokaci na revision symbol; chybějící
nebo jiná revize runtime neprojde linkerem ani při section GC.
`tools/luxir_build.py` připojí knihovnu automaticky. Backend může vložit precise shadow frames podle `Value`/`gc_heap`;
native PC stackmaps nejsou implementované. Driver neinicializuje heap za frontend a neslibuje shared-heap threads.
Windows/macOS tuto revizi zatím odmítnou před emitováním definic.

## Externí platformní služby

TLS a raw thread spawn, register snapshot/caller SP a Linux signal action,
mask a doručení jsou nyní dostupné pro Linux x86_64, arm64 a rv64.
Kontrakt struktur, vlastnictví, omezení statického ELF TLS a signal callbacků
je v [runtime/README.md](../runtime/README.md) a v hlavičkách `runtime/`.
Frontend může používat tyto moduly bez znalosti jejich syscallů či instrukcí;
musí přitom splnit zdokumentované ABI a životnost předávaných bloků.

Moduly lze stále explicitně připojit s `runtime_abi "none"`; ABI 1
je také připojí automaticky přes driver. Neimplikují integrovaný
GC, automatické safepointy, libc-kompatibilní raw threads nebo podporu
Windows/macOS. IR `thread(symbol)` již existuje; nová služba poskytuje
inicializované TLS bloky pro hlavní i nová raw vlákna.

Nový plán standardního managed runtime je v TODO. Požadavky prvního
frontendu jsou zaznamenány v [doc/lsl-runtime.md](lsl-runtime.md); nejde
o závazné univerzální ABI názvů `lsl_*`/`qbe_*`. LuxIR bude definovat vlastní
obecné standardní objektové ABI a implementovat jeho runtime; frontend
převede jazykovou sémantiku na toto ABI a předá potřebná metadata. Přenos
požadavků do dokumentace sám o sobě neznamená implementaci těchto služeb.

## Experimentální managed core (`runtime/managed.h/.c/.lir`)

Core se dá explicitně linkovat pod `runtime_abi "none"` nebo jako
ABI 1 profil s link guardem. Profil neobsahuje automatické stackmaps
ani STW. ABI 1 layout je: Value
64 bitů, tag v dolních třech bitech. Tag 1 je signed 61bitové celé číslo
(posun 3), 2 je None, 3/11 False/True. Nenulová hodnota s tagem 0 je
pointer na managed objekt. Nula je prázdný GC root, nikoli None.
Plný Int64 se mimo 61bitový rozsah boxuje; Float64 box ukládá přesný bitový
vzor včetně signed zero a NaN payloadu. Žádná globální tabulka objektů
není potřebná pro rozlišení typu. Raw pointery nejsou Value.

Hlavička objektu má 48 B: descriptor, capacity, next, mark/work link,
owner heap a payload_bytes, vše po 8 B. Payload následuje za hlavičkou.
Descriptor má 48 B: type_id, fixed reference_count, pointer na pole
offsetů, repeat_offset, count_offset, stride. Offsety jsou vůči payloadu.
Stride 0 vypíná opakovanou mapu; jinak jsou Value sloty v pravidelných
rozestupech a count se čte z count_offset. Metadata mají životnost nejméně
stejnou jako objekty; během kolekce se nesmějí měnit. User type IDs od 256.

Heap má 336 B, root frame 24 B (`previous`, `slots`, `count`). Každou
živou Value je před alokačním voláním nutné mít v propojeném root frame;
`object_new` může automaticky kolektovat. Kořeny/field mapy jsou přesné,
registry ani zásobník se konzervativně nehledají. Pop musí dodržet LIFO.
Objekty se nepřesouvají, takže payload pointer přežije GC, pokud objekt
zůstává dosažitelný. Hodnoty, root slots, descriptor pointery a jejich
životnosti musí být platné; API neslouží jako validator libovolných bitů
nebo nedůvěryhodných adres. Jiná halda v kořenech/polích je odmítnuta.

GC používá iterativní mark worklist uložený v hlavičkách, ne rekurzi a ne
alokovaný pomocný zásobník. Neplatná field mapa zastaví kolekci a resetuje
marky bez sweepu. Uvolněné bloky se vrací do size-class freelistů, slab
mapování se drží do `heap_destroy`; nejde o vracení neobsazených slabů OS,
kompaktování či generační GC. `heap_destroy` s aktivními frames vrací -16.

Halda má jediného vlastníka. Nepředávejte ji souběžným vláknům: STW a
thread registration zatím implementované nejsou. Chyby jsou explicitní
status a output slot, bez implicitních TLS exceptions. Při neúspěšné
alokaci output zůstává stejný; samotná kolekce může změnit stav haldy.

C reference se převádí developer nástrojem `tools/generate_runtime.py`
přes cproc commit `d1c53ddf56571573a7025324c8dd5c6d547a4d1f` a
`qbe2lir.py`. Uložené `.lir` soubory při sestavení nepotřebují cproc/C.
Freestanding LP64 typy jsou v `tools/runtime-include`, bez host libc
struktur. `--check` ověří, že generovaný IR není zastaralý.

## Managed řetězce a kolekce

`collections.h/.c/.lir` a `map.h/.c/.lir` navazují na tento experimentální
core. Driver s `--managed` je připojí automaticky; frontend inicializuje
heap, propojí root frames a volá běžné `luxrt_*` funkce. Žádný nový C
překladač ani cproc není potřeba pro sestavení z uloženého IR.

String payload: byte_length, codepoint_length, cached_hash (3×UInt64),
UTF-8 bajty a koncová nula. Bajtové NUL uvnitř jsou dovoleny; délka není
strlen. String je immutable, konstrukce ověřuje strict UTF-8; slice je
podle codepointů, concat/split udržují přesné kořeny během alokací. Split
s explicitním nenulovým oddělovačem zachovává prázdné části. Hash se
počítá jednou (FNV64 + avalanche); není kryptografickou obranou proti
koliznímu DoS. Unicode case mapping/interning/views zatím nejsou hotové.

Vector payload: length, capacity, managed backing Value. Backing má
length/count, capacity a data. List má precise opakovanou mapu pouze
živých Value slotů. IntArray/FloatArray mají raw 64bitové elementy,
ByteArray 8bitové; GC jejich obsah nepovažuje za pointery. Podporováno
new/zero, append, get/set, pop, length; List navíc clear. ByteArray
nepřijme hodnotu nad 255. Indexy jsou UInt64 s kontrolou mezí; jazykové
záporné indexy či coercion upraví frontend podle své sémantiky.

Map/Set payload: length, used, entry_capacity, index_slots, flags,
entries, hashes, index (8×64 bitů). Poslední tři fields jsou strong
managed roots. Entry backing jsou key/value dvojice v pořadí vkládání;
metadata a open-address index obsahují raw UInt64. Růst/kompaktování
přesouvá entries podle pořadí a používá uložené hashe bez čtení klíčů.
Delete vymaže strong Values, reinsert jde na konec; update zachová
původní key a pozici. Tombstones se odstraní při rebuild.

Default key policy rozlišuje typy. Volba `LUXRT_MAP_NUMERIC_KEYS` sdílí
Bool, Int a přesně integrální Float; například Int 2^53+1 a Float 2^53
zůstávají různé. Float +0/-0 jsou shodné, NaN pouze se stejným objektem.
Podporované keys jsou immutable skaláry a String; List/Map/Array jako
key jsou odmítnuty. API nemá implicitní LSL názvy ani chybové texty.
Chybějící key/konec iteratoru je -2. Mutace během cursor iterace není
povolená; invalidation/version tracking zatím není implementovaný.

Tato sada není celá standardní knihovna: Tuple, komplexní slice/mutace,
unicode case data, typové coercions a další funkce z požadavků prvního
frontendu zůstávají otevřené.

### Linux process handoff

`process.h` exports `luxrt_process_spawn` and `luxrt_process_wait` on all three
Linux64 targets. Spawn takes an explicit executable path, NUL-terminated argv
and envp, three borrowed stdio descriptors (or -1 to inherit), and a pid output.
NULL envp selects an empty environment; no PATH search or shell expansion occurs.
A single-threaded caller is required. Inherited stdio retains closed/CLOEXEC
state; explicit redirection clears child CLOEXEC. Parent descriptor flags stay
unchanged. Child setup blocks signals, preserves ignored handlers and the caller
mask, resets caught handlers, and closes every inherited descriptor above stdio.
The close_range fallback checks procfs identity before enumerating descriptors.
An internal CLOEXEC pipe reports setup/exec errors; failed children are reaped
and pid output is unchanged. Successful exec handoff is distinct from successful
program exit. Wait returns the raw Linux status plus exit code or 128+signal.
Concurrent bounded capture is provided separately by `capture.h`.

`tests/runtime/run_process.py` exercises C and emitted runtime IR, including
802 checks, forced procfs fallback, substituted filesystem rejection, closed stdio, explicit
environment, hundreds of inherited FDs, caught/ignored signals and failed exec
cleanup. RISC-V signal tests require QEMU with upstream fix
513823e7521a09ed7ad1e32e6454bac3b2cbf52d (QEMU 8.2.2 has an incorrect sigaction
layout that overwrites the caller's adjacent storage).

### Concurrent bounded process capture

`luxrt_process_capture(path, argv, envp, request, result)` uses caller-owned
request/result storage and byte spans. Size queries return 56 and 40 bytes;
LP64 field order/offsets are defined in `capture.h`. No frontend-specific values
or managed allocation are involved. The raw driver includes this module; unused
functions and data are removed by section GC.

The parent pumps input, stdout and stderr concurrently with nonblocking I/O and
ppoll. Input is a Unix stream socket and uses MSG_NOSIGNAL, so early child stdin
closure does not terminate the parent. Output/error are pipes. Counts report
accepted input bytes and captured output bytes; output need not be NUL terminated.
Exact-full buffers succeed if followed by EOF; an additional byte returns -28.
A zero timeout waits indefinitely; positive milliseconds (up to INT32_MAX) use
a monotonic deadline. Deadline measurement starts before spawn and includes
child setup/exec handoff through a nonblocking error channel. Kernel syscalls
and mandatory reaping after SIGKILL are not hard real-time bounded.

Capture creates a child process group. Capacity overflow, timeout (-110), or
communication error kills this group and reaps the direct child. A descendant
that explicitly changes group/session can escape this cleanup; capture is not
a process-tree sandbox. The leader is retained until pipes close, avoiding pgid
reuse before cleanup. SIGCHLD is blocked in the parent for capture and restored
after cleanup, while the child receives the original caller mask. Ignored
SIGCHLD or SA_NOCLDWAIT are rejected because wait ownership cannot be guaranteed.

The result is reset at entry and retains partial counts and wait status on error.
Status zero means communication/wait succeeded; a child's nonzero exit code is
reported separately. Valid, nonoverlapping spans and valid context pointers are
caller obligations. `tests/runtime/run_process.py --capture` compares a 1 MiB
input / 2 MiB output / 1 MiB stderr corpus, exact capacity and overflow cases,
closed stdin, timeout before/after stream closure, child mask/group inheritance,
error cleanup and descriptor/mask preservation.

`luxrt_process_start` exposes this asynchronous handoff independently for event
loops. A successful start returns a 24-byte uniquely owned handle (pid, error_fd,
bytes, record); it does not promise successful exec. Polling returns 1 pending,
0 handoff success, or negative setup/exec error. Terminal poll closes error_fd;
it never kills/reaps. Caller must prevent automatic/concurrent reaping, close
a pending channel on cancellation, and terminate/reap the child. Group creation
may still be pending before handoff, so cancellation also kills the direct pid.
Fragmented/malformed records and partial EOF are covered by the process corpus.
Capture fault injection stalls child exec and, separately, process group creation
for three seconds while requesting a 30 ms timeout, and verifies cancellation
before exec/group creation, direct-child SIGKILL status
and cleanup in C and emitted runtime IR.
