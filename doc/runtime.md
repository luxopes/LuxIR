# Kontrakt runtime LuxIR

Aktuální LuxIR vytváří assembler a neobsahuje integrovanou knihovnu luxrt.
Podporovaná deklarace je proto `runtime_abi "none"`; knihovnu pro externí
volání a její kompatibilitu stále zajišťuje frontend a linker.

`luxir --version` vypisuje podporovaný kontrakt. Čtečka přijme explicitní
`ir_version "1"` a `runtime_abi "none"`; neznámé verze odmítne před
zpracováním definic. Vstup bez hlavičky zůstává kompatibilní se stávajícím
frontendem LSL. Hlavičky se kontrolují pro každý vstupní soubor samostatně.

To je základ verzování, nikoli implementace luxrt nebo certifikace ABI
externího runtime. Budoucí číslované luxrt ABI musí popsat obecné služby, volání/chyby, OS vrstvu a způsob předávání
frontendových metadat (layouty, GC mapy a safepointy). Konkrétní layout
hodnot a objektů nesmí být pevně převzat z jednoho jazyka. Teprve poté se přidá
podporovaná revize a kontrola kombinace IR/runtime při linkování.


## Externí platformní služby

TLS a raw thread spawn, register snapshot/caller SP a Linux signal action,
mask a doručení jsou nyní dostupné pro Linux x86_64, arm64 a rv64.
Kontrakt struktur, vlastnictví, omezení statického ELF TLS a signal callbacků
je v [runtime/README.md](../runtime/README.md) a v hlavičkách `runtime/`.
Frontend může používat tyto moduly bez znalosti jejich syscallů či instrukcí;
musí přitom splnit zdokumentované ABI a životnost předávaných bloků.

Tyto moduly nemění `runtime_abi "none"`: stále jde o explicitně připojené
externí služby a sestavení přes assembler/linker. Neimplikují integrovaný
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

Zatím se explicitně linkuje pod `runtime_abi "none"`. Není deklarován
jako dokončené integrované ABI 1. Layout v této vývojové větvi je: Value
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
