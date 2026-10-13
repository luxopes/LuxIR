# LuxIR TODO

Cíl: frontend libovolného jazyka (LSL je první uživatel) řeší jen jazyk a zapíše jednotný LuxIR. Vše strojové a
platformní řeší LuxIR: optimalizace, generování kódu, obecné runtime služby,
přístup k operačnímu systému, formát binárky a linkování. Jeden `.lir` z LSL
pak dá spustitelný program pro Linux, Windows, macOS i Android.
Navazující plán frontendu: sekce „Plán 0.10“ v `TODO.md` repozitáře
LuxAI/LSL.

Zdrojem pravdy zůstává C reference v `src/`; `lsl/luxir.lsl` se přegeneruje
(`make lsl`). Každá položka platí až s testy v obou verzích (C i LSL dají
stejný výstup) a s měřením: programy LSL přeložené přes LuxIR nesmí být
pomalejší než předchozí vydání LSL, při překladu ani za běhu.

## Standardní runtime pro frontendy (první uživatel LSL) — 13. 10. 2026

Rozhodnutí: LSL bude jen frontend (LSL → LuxIR). Celý runtime patří LuxIR:
model objektů, alokace a GC, kolekce, řetězce, převody čísel, chyby, I/O,
procesy, vlákna, pro každou architekturu a systém. Rozhraní standardizuje
LuxIR, LSL se mu přizpůsobí. ABI je obecné pro libovolný frontend;
LSL-specifická sémantika a názvy nejsou povinným kontraktem ostatních jazyků.
Požadavky a dnešní rozhraní (145 funkcí):
`doc/lsl-runtime.md`; referenční kód slovníku a řetězců v LuxIR:
`doc/lsl-runtime/`; referenční chování: x86 emitory runtime v repozitáři LSL.

- [ ] **Návrh rozhraní:** standardní managed ABI (hodnoty, tagy, layouty,
  chyby, GC kořeny) a seznam funkcí; verze v hlavičce `runtime_abi`.
- [ ] **Implementace pro Linux x86_64** se stejným chováním a výkonem jako
  dnešní runtime LSL (měřítka v `doc/lsl-runtime.md`).
- [ ] **Linux arm64 a rv64**, pak Windows a macOS.
- [ ] **Předání LSL:** jakmile je rozhraní hotové, LSL přepne `qbe_emit` na
  něj a odstraní své x86 emitory, `runtime.o` a přímý backend.

## Požadavky LSL pro přesun runtime do LuxIR — 13. 10. 2026

LSL přepisuje svůj runtime (dnes ~10 000 řádků ručně generovaného x86-64:
GC, alokátor, řetězce, kolekce, převody, I/O, procesy, vlákna) do LSL/LuxIR
nad luxrt, aby LSL generovalo jen přenositelný LuxIR. Ověřeno proti
`doc/ir.md` a `runtime/` verze 0.1.1 a proti tomu, co runtime LSL používá.
Bez blokujících položek nejde runtime na Linuxu přeportovat; ostatní mají
pomalejší náhradu, kterou LSL do té doby použije.

Tyto služby jsou obecné: nevyžadují layout objektů ani chybový model LSL.

Blokující (Linux x86_64, arm64, rv64):
- [x] **TLS ve vláknech:** `thread` data fungují pro hlavní vlákno, ale
  vlákna z luxrt nemají TLS blok. Runtime LSL drží v TLS čekající chybu a
  její příznak (zapisuje se z I/O, procesů, haldy) a vrchol zásobníku vlákna.
  Potřeba: vytvoření vlákna s inicializovaným statickým TLS (fs / tpidr_el0 /
  tp), přístup přes `thread(x)` v obou vláknech.
  Ověřeno: 40 raw vláken čte/zapisuje vlastní IR `thread(...)`, inicializovaná
  data + zarovnaná BSS; C/LSL shoda na všech třech cílech. Standalone
  static PIE také inicializuje TLS hlavního vlákna bez libc/loaderu.
  Jde o statické local-exec ELF TLS; nikoli libc TLS/DTV nebo dlopen.
- [x] **Uložení registrů a ukazatel zásobníku:** konzervativní GC LSL musí
  najít ukazatele i v registrech. Potřeba primitivum (instrukce nebo luxrt
  funkce pro každou architekturu, obdoba setjmp), které uloží callee-saved
  registry do paměti a vrátí aktuální ukazatel zásobníku; plus vrchol
  zásobníku hlavního vlákna ze startu.
  Ověřeno: `luxrt_capture_regs`, caller SP a kernel stack top; konzervativní
  testovací scanner najde alokaci, jejíž jediný root v assemblerovém fixture
  je v callee-saved registru. Není tím implementován produkční GC ani
  stop-the-world; scanner musí procházet snapshot i živý zásobník.
- [x] **Signály:** `rt_sigaction` s `sa_restorer` (návratová rutina pro každou
  architekturu), maska signálů; runtime LSL instaluje obsluhu při startu.
  Ověřeno: C i IR handler se vrátí, blokovaný signál se doručí po odmaskování,
  původní action/maska se obnoví. RV64 používá kernel/VDSO restorer ABI.
  Implementován Linux kontrakt, nikoli přenositelná správa signálů všech OS.

Výkonové (náhrada existuje, ale je pomalejší):
- [x] **Atomiky jako instrukce:** atomické add/xchg/cmpxchg/load/store a
  bariéry v IR (dnes jen volání do `luxrt/*/atomic.S`). Používají je zámky,
  vlákna, GC a vestavěné `atomic_add`, `atomic_xchg`, `atomic_cas`. Nové
  SC instrukce jsou inline pro šest cílů; C/LSL execution na třech Linux
  cílech, souběžný CAS, store-buffering a bariéry běžné paměti. Varianta
  s jemnějšími memory orders a optimalizace CAS packetu zůstávají otevřené.
- [x] **`sqrt`** pro Float64 (a Float32) jako instrukce; vestavěné `sqrt`
  a modul `math`. C/LSL execution a zaokrouhlování ověřeno na Linux
  x86_64/arm64/rv64; assembler validní pro všech šest cílů.
- [x] **Horní polovina násobení 64×64 → 128** (`umulh`, `mulh`): přesný
  převod textu na Float (Eisel-Lemire), hashe. Implementované nativně;
  izolovaný x86_64 umulh loop je 2,96× rychlejší než čtyřnásobení fallback.
  Nejde o měření celého Float parseru ani skutečného programu LSL.
- [x] **clz, ctz, popcount** jako instrukce: vestavěné `popcount`,
  `trailing_zeros`, převody Float. Zero výsledky definované; baseline ISA
  nevyžaduje POPCNT/Zbb. C/LSL shoda, 1 110 145 kontrol na každý Linux cíl.
- [x] Omezené inlinování malých scalar leaf funkcí: volitelné `-i`,
  definice před voláním ve stejném vstupu, jeden blok, nejvýše 8 operací
  a 6 parametrů. Lokální vazbu symbolů musí garantovat volající; default off.
  120 000 kontrol na variantu, šest assemblerů, runtime na třech Linux cílech.
  Rozšíření na více bloků a interprocedurální analýzu zůstává otevřené.

Windows a macOS (navíc):
- [ ] OS vrstva luxrt pro Win32 a libSystem, TLS na Windows (dnes odmítnuto),
  výstup PE a Mach-O (vlastní linker nebo externí nástroje), testy na
  skutečném systému.

## Rozhraní pro frontendy

- [x] Základ verzování IR: `luxir --version`, volitelné hlavičky
  `ir_version "1"` a `runtime_abi "none"`, kontrola neznámých, opakovaných
  a pozdních hlaviček. Legacy IR zachovává výstup. C reference a
  přegenerovaná LSL verze mají shodné výstupy/diagnostiku; viz
  `tests/versioning.py` a `VALIDATION.md` (12. 10. 2026).
- [x] Číslovaný Linux64 explicit-root profil ABI 1 a retained link guard
  `luxrt_abi_v1_le64_explicit`; automatické připojení runtime driverem,
  size queries a manifest 156 public scalar funkcí. `none` zůstává
  legacy/external. Automatické stackmaps, STW a další OS zůstávají níže.
- [x] Descriptor pointer maps a compiler `Value`/`gc_heap` metadata:
  backend generuje přesné shadow-root slots pro parametry/lokály/Phi,
  pop na normal/error return, no-call leaf elision. 60 002 kontrol
  s forced GC na třech Linux cílech. Lokály žijí do přepisu/return.
- [ ] Native PC/register stackmaps, automatické last-use clearing,
  safepoint coordination a STW shared-heap vlákna. Shadow roots nejsou
  touto optimalizovanou implementací; borrowing owner musí zůstat rooted.
- [x] Status `invoke` call s explicitní success/error hranou v IR;
  typed Int32 nebo discarded status, direct/extern/indirect callees,
  žádný skrytý TLS příznak. Backend sníží na call/branch před SSA a
  foldne known status při inlinování. Není native unwind/automatický cleanup;
  frontend zajišťuje error-block root/resource cleanup.
- [ ] Kontrolovaná aritmetika jako instrukce (přetečení Int, dělení nulou,
  kontrola mezí) s cílem chyby; backend kontroly slučuje a odstraňuje.
- [ ] Dokumentovat celé rozhraní v `doc/runtime.md` vedle `doc/ir.md`.

## Sestavení přes externí nástroje — vývojová 0.1.2

- [x] `tools/luxir_build.py`: raw IR main → static PIE na třech Linux cílech,
  automaticky raw luxrt, main TLS, relativní datové/TLS pointer relokace,
  skutečná ochrana RELRO, NX stack a GC nepoužitých funkčních sekcí.
  C/LSL binárky shodné; 30 kontrol na každý cíl včetně selhání při zápisu
  do RELRO a zachování existujícího outputu po neúspěšném překladu.
  Používá Python a GNU as/ld při sestavení; výsledná binárka je nepotřebuje.
  Není tím hotový managed runtime, vlastní object writer nebo linker.
- [x] Odstranění nepoužitých jednotlivých datových tabulek, BSS, TLS a
  numerických literálů: ELF backend `-s`, driver a GNU ld `--gc-sections`.
  Relokace zachovají živé řetězce pointerů i callbacky; explicitní sekce
  frontendu zůstávají společné. Common symboly ponechány GNU linkeru.
  Vlastní linker a vyřazování uvnitř explicitních sekcí tím hotové nejsou.

## Výstup objektů a linkování (bez binutils)

- [ ] Vlastní assembler do objektů: x86_64 ELF64, pak COFF (Windows),
  Mach-O (x86_64 a arm64), arm64 a rv64 ELF.
- [ ] Vlastní linker:
  - statický PIE ELF se stejnými zárukami jako dnešní `ld` v LSL
    (`-static -pie`, `-z text`, `-z noexecstack`, bez build-id, strip);
  - PE32+ s ASLR, DEP a high-entropy VA, importy `kernel32` a `ws2_32`;
  - Mach-O s `LC_MAIN`, `libSystem` a ad-hoc podpisem (na arm64 povinný).
- [ ] `luxir -t <cíl> -o program program.lir runtime.lir …`: od `.lir` po
  spustitelný soubor jedním příkazem, bajtově deterministicky.
- [ ] Odstranění mrtvého kódu celého obrazu včetně runtime (dnes se
  `runtime.o` linkuje celý; tabulka mocnin pro Float zvětšila v LSL 0.9.2
  každou binárku o ~12 KB).
- [ ] Akceptace: linuxové binárky LSL bez `as` a `ld` projdou branou
  hardeningu obrazu v release preflightu LSL a celou sadou testů.

## Implementované základy runtime — 12. 10. 2026

Jde o testované externí moduly, nikoli o dokončený integrovaný luxrt.
Kontrakt, omezení a příkazy: [runtime/README.md](runtime/README.md).
Následující úzké části jsou hotové; širší položky níže zůstávají otevřené.

- [x] Raw byte-span `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`,
  FNV-1a hash a strict UTF-8 validace: `runtime/memory.lir`, C reference.
  235 296 kontrol; C/LSL assembler shodný pro šest cílů; execution Linux
  x86_64 nativně, arm64 a rv64 v QEMU.
- [x] Desetinné Int64 ↔ text v celém rozsahu s kontrolou overflow/kapacity
  a nezměněným výstupem při chybě: `runtime/integer.lir`. 3 800 329 kontrol.
- [x] Raw checked add/sub/mul/floor-divide s chybovým status a výstupním
  slotem: `runtime/checked.lir`. 800 512 kontrol proti 128bitové referenci.
  Nové checked IR instrukce/optimalizace kontrol tím ještě nejsou hotové.
- [x] Linux x86_64 map/unmap/protect, openat/read/write/seek/close,
  getrandom/clock/nanosleep přes skutečné syscalls: `runtime/linux-x86_64/`.
  27 kontrol základů a 138 rozšířených kontrol souborů, procesů, socketů,
  terminálu a futexu. Arm64/rv64 mají trap a OS wrappery, základní OS test
  běží v QEMU. Vyšší spawn/TLS/signálová politika stále není hotová.
- [x] Standalone static PIE demo bez libc/loaderu, C/LSL binárka shodná,
  neexecutable stack: `examples/runtime-demo.lir` a
  `tests/runtime/standalone.py`. Zatím sestaveno GNU as/ld.
- [x] Kernel entry, argc/argv/envp a AT_PAGESZ + getenv: `startup.c/.lir`,
  10 009 kontrol; libc-free static PIE program skutečně spuštěn na všech
  třech Linux architekturách (arm64/rv64 v QEMU).
- [x] I/O helpery s retry EINTR a zachováním částečného count na chybě:
  `io.c/.lir`, 33 kontrol včetně skutečného signálu a pipe přes 300 KB.
- [x] Externí SC atomiky load/store/add/swap/CAS/fence 32/64-bit: C11 oracle
  a assembler pro Linux x86_64/arm64/rv64, 2 032 kontrol na každém cíli.
  Stále nejde o IR instrukce, nezávislý plnohodnotný memory model frontendu.
- [x] Raw clone/join Linux x86_64: 40 guarded-stack vláken, 4 000 000
  inkrementů; C/LSL shoda. Legacy callback nesmí používat libc/TLS/managed objekty; nový
  TLS spawn na třech architekturách podporuje pouze statické local-exec TLS.
- [x] rv64 PC-relative adresy pro static PIE přes explicitní `-p`:
  lokální i externí symboly + offset, C/LSL shoda a spuštění v QEMU.
  Dynamické datové relokace a vlastní linker tím nejsou implementovány.
- [x] Linux x86_64 self-host stage 1 == stage 2 == stage 3: 48,27 s a
  46,16 s, obě pod limitem 60 s v tomto prostředí; `tests/selfhost.py`.
- [x] Linux lokální testová matice, C/LSL execution arm64/rv64 + assembler
  šesti targetů: `tools/check_linux.sh`. Nepotvrzuje výkon skutečných CPU.
- [ ] Dokončit běh `.forgejo/workflows/linux-reference.yml` na remote CI.
  Push vytváří běhy; run 13 je `waiting`, předchozí jsou `cancelled`
  (ověřeno API 13. 10. 2026). Úspěšný job zatím potvrzený není.
  Workflow ověřuje C reference, nikoli vlastní self-host na všech platformách.
- [ ] Integrovat tyto moduly s aktuálním frontendem/layoutem LSL, doplnit
  výkonnostní brány a skutečné cílové execution testy; nenahrazovat celou
  širší položku níže stavem hotovo podle samotné existence prototypu.

- [x] Raw caller-owned arena allocator (`runtime/arena.c/.lir`): zarovnání,
  checked kapacita/adresní rozsah, rewind, bez syscallů v alokační cestě.
  1 213 384 kontrol C/LSL, sanitizery C, standalone execution tří Linux
  cílů. Nejde o managed allocator, bump nursery s GC nebo thread STW.

- [x] Experimentální managed core: tagované 61bitové Int, Bool/None,
  bezeztrátové Int64/Float64 boxy, descriptor pointerových polí a opakovaných
  Value slotů, explicitní root frames, non-moving mark-sweep a slab/freelist
  allocator. C → IR generování, 1 281 292 kontrol, tři Linux cíle.
  Jeden vlastník haldy; automatické stack mapy, generační GC, STW,
  finalizéry a stabilní integrované ABI nadále otevřené.

- [x] Experimentální managed UTF-8 String (cached hash, equality/order,
  concat, codepoint slice, explicit split), dynamic Value list, raw
  Int64/Float64/Byte vectors, insertion-ordered Map a Set; 799 669
  modelových kontrol, GC stress a tři Linux execution cíle.
  Jde o základní API, ne celý stdlib/145 funkcí, Tuple nebo Unicode case.
  String lookup v izolovaném benchmarku 27–29 ns (4096 různých stejně
  obsahujících objektů); není to end-to-end benchmark autobusů v LSL.
  Doplněno volitelné per-heap SipHash-2-4 (128bit klíč), cached String hash,
  hash numerických klíčů a lookup/read-only delete podle String z jiné haldy.
  Seed lze měnit jen bez živých objektů; soukromé slab metadata zachovává
  kontext ABI 1 (336 B). 54 982 kontrol/implementaci, 64 oficiálních vektorů,
  3 005 nezávislých Python referencí a tři Linux cíle. `luxrt_heap_init_secure`
  získá klíč z OS náhody; 16 fault kontrol (partial/EINTR/zero/error/OOM),
  zachování outputu a fail-closed politika. Deterministický defaultní initializer
  zůstává kompatibilní a není collision-flood ochrana.

- [x] Raw Float64 bits ↔ text: shortest/nearest Python-repr rendering,
  strict ASCII decimal parser, round-to-nearest ties-even, Ryu short route
  a přesné midpoint porovnání arbitrárně dlouhých vstupů (bounded digits
  + sticky). 86 321 kontrol/implementaci, Fraction/Python oracle,
  standalone execution tří Linux cílů. Není Eisel-Lemire fast path,
  Float32 parser nebo end-to-end LSL brána. Managed adapters viz níže.

- [x] Managed String ↔ Int64/Float64 adapters nad strict decimal API,
  bez implicitních Bool/numeric coercions; immutable Tuple s přesnou GC
  tail mapou, slice/concat a snapshot Value listu. 1 530 264 kontrol,
  nucené kolekce, owner/type/OOM zachování outputu, tři Linux cíle.
  Není tím doplněné strukturální equality/hash Tuple ani celý stdlib.

## Runtime knihovna LuxIR (luxrt)

### OS vrstva: jediné místo s kódem pro konkrétní systém
- [ ] Paměť: map, unmap, protect, velikost stránky.
- [ ] Soubory a adresáře: open, read, write, seek, close, stat, list,
  rename, unlink, mkdir, chmod, symlink, cesta k programu.
- [ ] Procesy: spawn s argv, env a pipes; wait, exit, kill; zavírání
  zděděných deskriptorů.
  Linux64: implementováno `luxrt_process_spawn/wait`, explicitní argv/env,
  přesměrování stdio bez změny rodičovských FD, CLOEXEC error handshake,
  close_range + ověřený procfs fallback, obnova masky/reset caught handlers,
  úklid selhaného exec a dekódování exit/signal status. Volající je zatím
  jednovláknový. Doplněno souběžné bounded capture stdin/stdout/stderr, process
  group, SIGPIPE-safe vstup, maska SIGCHLD a úklid při limitu/timeoutu. Přes
  3,1 milionu kontrol na každém Linux cíli (C/LSL IR, sanitizéry a raw binárky).
  Asynchronní handoff `luxrt_process_start/handoff_poll` pokrývá deadline i během
  child setup/exec. Fault injection: exec zpožděný o 3 s, timeout 30 ms, dítě
  ukončené/reapnuté před exec; fragmentované/chybné protokoly. Kernel syscall a
  reap nejsou hard real-time bounded. Ostatní OS stále zbývají.
- [ ] Vlákna: spawn a join, čekání na adrese (futex / WaitOnAddress /
  ulock), TLS.
- [ ] Čas, spánek, OS náhoda, proměnné prostředí, argumenty, pracovní adresář.
- [ ] Sockety TCP, UDP a unix; síťové chyby.
- [ ] Terminál: isatty, raw režim, velikost okna, Ctrl-C.
- [ ] Jednotný chybový model (kód a text) pro všechny systémy.

Implementace OS vrstvy:
- [ ] Linux přes systémová volání: x86_64, arm64, rv64.
- [ ] Windows přes Win32: `kernel32`, `ws2_32`, `bcrypt` pro náhodu.
- [ ] macOS přes `libSystem`: systémová volání macOS nejsou stabilní ABI.
- [ ] Android: Linux ARM64 se zohledněním omezení (seccomp, chybějící `/etc`,
  Termux i APK).

### Obecný runtime jazyka: převzít to, co dnes LSL generuje ručně ve strojovém kódu x86_64
- [ ] Alokátor a přesný GC nad stack mapami (viz Rozhraní), safepointy a
  rozpočet alokací; vícevláknový běh.
- [ ] Číselné převody:
  - Int ↔ text;
  - Float ← text (Clinger + Eisel-Lemire + přesné porovnání dlouhých vstupů,
    převzít z LSL 0.9.2);
  - Float → text (nejkratší a nejbližší zápis shodný s `repr` v Pythonu).
- [ ] Paměťové a řetězcové primitivy: memcpy, memmove, memcmp, hledání bajtů,
  UTF-8, hash; vektorizace tam, kde ji cíl umí.
- [ ] Atomiky a fence jako instrukce pro všechny cíle.
- [ ] Výpis neodchycených chyb a ukončení programu (dnes runtime LSL).

## Výkon runtime (benchmark „autobusy“, 12. 10. 2026)

Program: dosažitelnost v grafu ~100 000 zastávek a ~780 000 hran, 150 dotazů
DFS, zastávky jako řetězcové klíče slovníku (`~/autobus.lsl`, vstup
`04.in`). C se stejným algoritmem 0,33 s, Python 8,8 s, LSL 0.9.2 19 s
(16,4 s s úpravou indexu slovníku níže). Stejný algoritmus v LSL s čísly
zastávek a `IntArray` místo slovníků: 1,48 s. Cíl: řetězcová verze nejvýš 3 s,
číselná nejvýš 0,6 s.

- [ ] **Tagované hodnoty:** hodnoty Any dnes nenesou tag, takže runtime
  každou hodnotu ověřuje dotazem do globální hashovací tabulky objektů
  (`runtime_is_heap_object`). To je výpadek cache při každém hashi klíče,
  porovnání i kontrole typu: jedna lookup ve slovníku ~90–120 ns, z toho
  většina na tuto tabulku. Navrhnout reprezentaci s tagem (tagované ukazatele
  nebo malé Int v tagu), aby test „je objekt / je String“ byl test bitu.
  Patří do kontraktu s frontendem (layout hodnot) a do GC.
  Přijetí: lookup slovníku s řetězcovým klíčem nejvýš 40 ns.
- [ ] **Hash v řetězci:** ukládat hash do hlavičky String (spočítá se jednou),
  porovnávat nejdřív ukazatel, délku a hash, teprve pak bajty.
- [ ] **Slovník v runtime:** kompaktní slovník se zachovaným pořadím vkládání
  (jako CPython). V položce indexu uložený hash a druh klíče; při růstu
  přesun položek bez čtení klíčů; jedno hashování při vložení. LSL 0.9.2 to
  po vydání doplnilo do svého runtime (`heap_runtime_linux_x86_64.lsl`,
  `runtime_dict_probe`, `runtime_dict_index_insert`, `runtime_dict_grow`,
  `runtime_dict_set`) a slouží to jako reference. Malé slovníky bez indexu
  zatím porovnávají lineárně přes `runtime_equal_any`.
- [ ] **Množiny a sety:** stejné principy jako slovník (`set`, `in` nad Set).
- [ ] **split a malé řetězce:** `split` vytváří nový objekt pro každý token.
  Zvážit internování krátkých řetězců nebo řezy bez kopie, aby opakované
  klíče sdílely objekt a porovnání skončilo na shodě ukazatelů.
- [ ] **Alokace:** každý nový objekt se zapisuje do globální tabulky
  objektů (`runtime_register_object`), kterou pak potřebuje rozlišení
  netagovaných hodnot. Seznam `[i]` stojí 336 ns (Python 169 ns), `split("=")`
  krátkého řetězce se třemi alokacemi 342 ns i s nativní cestou. Benchmark
  „vrah“ (milion řádků `otazka=odpoved`, seskupení do slovníku seznamů): C
  0,31 s, Python 3,8 s, LSL 0.9.2 4,9 s, se změnami po vydání 2,8 s.
  S tagovanými hodnotami tabulka odpadne; alokátor s bump pointerem a
  generační GC (dnes každý cyklus prochází celou haldu, úplný `gc()` nad
  3 miliony objektů ~0,6 s).
  Přijetí: malá alokace nejvýš 30 ns, „vrah“ nejvýš 1 s.
- [ ] **Benchmarková sada:** přidat „autobusy“ (řetězcovou i číselnou verzi)
  a mikrobenchmarky slovníku (vložení, `in` při zásahu i minutí, čtení,
  vkládání do nového slovníku, append/pop na seznamu) s referencí v C a
  Pythonu; měřit při každé změně runtime.

## Optimalizace (převzít z frontendu a doplnit)

- [ ] Rozšířit bounded `-i` na více bloků, forward/cross-module calls
  a interprocedurální specializaci. Jednoblokové scalar leaf funkce jsou
  implementované výše; end-to-end LSL `function_calls` benchmark a integrace
  volby do cizího frontend driveru zůstává otevřená.
- [ ] Smyčky: kanonická indukční proměnná a zjednodušení podmínky konce. Má to
  být obecná náhrada podmínky `rang` s literálovým krokem, kterou dnes dělá
  `qbe_emit` v LSL. Dále LICM, strength reduction a odstranění kontrol mezí.
- [ ] Propagace konstant a specializace přes volání (dnes guarded
  specializace ve frontendu LSL), odstranění mrtvých větví.
- [ ] Rozvíjení redukčních smyček a vektorizace: nový kontrolovaný opera
  benchmark `benchmarks/contest/` potvrzuje scalar/unroll/SIMD rezervu.
  Backend numeric 1,39× a text parse 1,38× C -O2; není to LSL end-to-end.
  Samotná změna block layoutu nepomohla a byla vyřazena.
- [ ] Ke každé optimalizaci: diferenční test proti verzi bez ní a benchmark LSL.

## Cíle a testy

- [ ] `amd64_win`: TLS (dnes se v Wine testech odmítá), velké rámce zásobníku
  (`__chkstk`), SEH tabulky pro výpis chyb.
- [ ] `amd64_apple` a `arm64_apple`: poprvé spustit na skutečném Macu (dnes
  jen ověřený assembler; testy spustí uživatel).
- [ ] CI matice: Linux x86_64 nativně, arm64 a rv64 v qemu-user, Windows ve
  Wine a před vydáním na skutečném Windows, macOS na skutečném Macu.
- [ ] Self-host LuxIR na každém cíli: `luxir` přeložený LSL a luxrt pro daný
  systém, bajtově stejný výstup `.s` jako na Linuxu.
- [ ] Aktuální minutová selfhost brána v obnoveném prostředí: profilovaný
  překlad 63,76 s, z toho LuxIR backend 3,67 s. Fixed point ověřen, ale
  limit 60 s nesplněn; předchozí commit zde také timeoutoval. Zbylé fáze
  nejsou jednotlivě profilované (`validation/data-sections-profile.json`).
- [ ] Výkonové měření backendu: čas překladu LSL compileru přes LuxIR
  nesmí růst s přidanými optimalizacemi bez vyrovnání jinde.
