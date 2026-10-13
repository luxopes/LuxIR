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
These calls do not yet supply concurrent buffered capture or a deadline.

`tests/runtime/run_process.py` exercises C and emitted runtime IR, including
779 checks, forced procfs fallback, substituted filesystem rejection, closed stdio, explicit
environment, hundreds of inherited FDs, caught/ignored signals and failed exec
cleanup. RISC-V signal tests require QEMU with upstream fix
513823e7521a09ed7ad1e32e6454bac3b2cbf52d (QEMU 8.2.2 has an incorrect sigaction
layout that overwrites the caller's adjacent storage).
