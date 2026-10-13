# Externí základ luxrt (vývojová fáze)

Tyto moduly jsou skutečný kód v LuxIR, se samostatně kompilovanou C referencí.
Nejsou zatím integrovány do managed runtime LSL ani do jeho GC. Používají
`ir_version "1"`, `runtime_abi "none"`: tento kontrakt nadále přesně uvádí,
že backend nespravuje plnohodnotné ABI luxrt. Nejde o ABI 1, které by bylo
možné bezpečně podsunout dnešnímu frontendu LSL.

## Moduly

- `memory.lir` / `memory.c`: raw byte-span memcpy, memmove, memset,
  lexikografické unsigned memcmp (-1/0/1), memchr, FNV-1a 32bit hash a
  UTF-8 validace (včetně přebytečných zápisů, surrogate rozsahu a limitu
  U+10FFFF). Hash je nekryptografický; není návrhem hashování
  nedůvěryhodných klíčů veřejného slovníku.
- `integer.lir` / `integer.c`: striktní desetinný parse Int64 a formátování
  celého rozsahu včetně Int64.MIN. Native formatter zpracuje dvě číslice na jedno unsigned dělení;
  rozklad zbytku 0..99 používá přesnou identitu `(r*205)>>11`. Žádný implicitní whitespace/prefix,
  parser přijímá +/-, formatter nepřidává NUL. Na chybě výstup nezměněn.
- `checked.lir` / `checked.c`: kontrolované add/sub/mul/floor-divide.
  C reference nepoužívá signed-overflow UB, test má nezávislou 128bitovou
  referenci. Toto jsou volané pomocné funkce, nikoli nové IR instrukce ani
  landing pads; kontroly zatím nejsou slučovány/odstraňovány backendem.
- `linux-x86_64/os.lir` / `os.c` / `syscall.S`: přímé Linux syscalls pro
  map/unmap/protect, openat/read/write/seek/close, getrandom, clock_gettime
  a nanosleep. Doplněny jsou statx, getdents64, mkdir/unlink/rename/chmod,
  symlink/readlink, cwd/chdir, pipe/dup, fork/exec/wait/kill/exit, close_range,
  TCP/UDP/Unix sockety, ppoll, ioctl a futex. Open vždy používá O_CLOEXEC. Krátké read/write/random a
  přerušení EINTR jsou vráceny volajícímu, který je musí obsloužit.
  `start.S` je minimalistický korektně zarovnaný SysV entrypoint pro demo.
  OS runtime nevolá libc; libc se používá pouze v testovacích driverech.

## Raw ABI a chyby

Předpokládá se platný rozsah paměti, 64bitový adresní prostor a délky jako
uint64. Paměťové API neprovádí kontrolu objektového layoutu ani GC typů.
Nulová délka nepřistupuje do paměti a umožňuje NULL. memcpy vyžaduje
nepřekrývající rozsahy, memmove podporuje překryv i identické adresy.
Kopírování má 64bitovou cestu pouze pro oba zarovnané pointery; další
bajty a nezarovnané vstupy se kopírují po bajtech. Nečte přes konec rozsahu.
Široká SIMD vektorizace je stále otevřená a není vydávána za dokončenou.

Parse: 0 = úspěch, -22 = syntax, -34 = rozsah. Format: počet zapsaných
bajtů, -28 = nedostatečná kapacita. Checked arithmetic: 0 = úspěch,
-34 = overflow, -33 = dělení nulou. Všechny chyby zachovají výstupní slot.
OS vrací nezáporný výsledek nebo záporné Linux errno; map vrací raw adresu
jako Int64. Tento Linux model není ještě jednotný chybový model ostatních OS.
Velikost stránky získá `startup.lir` z Linux AT_PAGESZ; nehádej ji z architektury.
Původní x86_64 demo nadále používá 4096, nové startup demo skutečné AT_PAGESZ.

## Testování

```sh
make check-runtime LSLC=/cesta/k/lslc LSLC_BACKEND=/cesta/k/luxir
```

C a LSL backend musí generovat bajtově stejný assembler runtime modulů
pro všech šest cílů. Paměťové, číselné a checked testy jsou nativně spuštěny
na Linux x86_64; Linux arm64 a rv64 navíc prošly execution testy v qemu-user; Windows/macOS nikoli. C reference
prošla ASan/UBSan. LeakSanitizer je v tomto hostu nedostupný kvůli inspekci
/proc/ptrace; testy ho explicitně vypínají, netvrdí ověření leaků.
Native assembler je testován canaries a guard pages u paměťového modulu;
není vydáván za instrumentovaný ASanem.

`tests/runtime/standalone.py` skutečně vytvoří a spustí statický PIE program
bez libc a bez ELF interpreteru s neexecutable stackem. C/LSL backendy dají
identickou binárku. Používá GNU as/ld; vlastní assembler/linker tím není hotov.

## Co zůstává otevřené

Integrace hodnot s tagy, objektové mapy, stack maps/safepointy, GC a vlákna,
Float převody, kontejnery, platformní implementace Windows/macOS/Android,
další platformní funkce, vlastní object writer/linker a optimalizace.
Neexistuje automatické napojení těchto helperů do LSL 0.9.2. Pro přenos
konkrétního runtime/layoutu je nutný aktuální zdroj frontendu a jeho plán 0.10.
Benchmark `autobus.lsl`/`04.in` v přiloženém archivu není.

## Rozšíření Linux (12. 10. 2026)

`linux-arm64/` a `linux-rv64/` obsahují vlastní trap i OS wrappery s čísly
syscallů asm-generic. Každá sada používá šest argumentů, raw negativní errno,
CLOEXEC na nových deskriptorech, MSG_NOSIGNAL při socket send a pointery na
Linux UAPI struktury. `stat` bere `struct statx`, `list` vrací getdents64:
layouty nejsou automaticky přeložené do frontendových objektů. Fork je pouze
x86_64 a vyžaduje jednovláknového volajícího. Nízké syscall wrappery
nenahrazují hotový spawn s převody argv/env, nastavováním pipes a hygienou
všech zděděných deskriptorů. `close_range` může vrátit ENOSYS.

`startup.c/.lir`: argc, argv, envp a AT_PAGESZ z platného kernel entry stacku;
getenv vrací vypůjčený pointer, odmítá prázdné jméno a `=` v něm. Vstupy jsou
NUL-terminated; argumenty ani prostředí se nekopírují. `entry.S` zavolá
`luxrt_entry_main(stack)` a poté exit_group. Nový entrypoint je samostatný
kontrakt; původní demo start.S zůstává kompatibilní. Static PIE demo nemá
libc ani ELF interpreter a skutečně běží pro x86_64, arm64 i rv64. Test
používá GNU as/ld, ne vlastní assembler/linker.

`io.c/.lir`: read_full a write_full opakují EINTR; při jiné chybě zachovají
počet již přenesených bajtů. Read EOF vrací 0 s kratším count, write bez
pokroku -EIO. EAGAIN se nebusy-waituje. Test zahrnuje pipe přes 300 KB,
short read, částečný přenos před EAGAIN a skutečně přerušené read přes signál.

`atomic.c` je nezávislá C11 reference. `atomic.S` pro tři Linux architektury
má 32/64-bit load, store, fetch-add, swap, CAS (vrací starou hodnotu) a fence.
Vše je sequentially consistent; přirozené zarovnání je povinné. Atomiky
nemají alokace ani globální mutex. Jsou externí funkce, nikoli nové IR
instrukce. Testuje se contention čtyř vláken, modulo přetečení, oba výsledky
CAS a store-buffering litmus. Všechny tři Linux implementace prošly spuštěním.

`linux-x86_64/thread.c/.lir/.S`: raw clone na stacku volajícího a join přes
CLONE_CHILD_CLEARTID/futex. Stack, callback a tid slot musejí žít do join.
Legacy callback nesmí používat libc, compiler TLS, unwinding nebo managed
objekty. Nový TLS spawn níže podporuje statické TLS; GC není implementován. Tento modul není plnohodnotné vlákno jazyka LSL.
Guard page poskytuje caller; test má guarded stacky a čtyři miliony inkrementů.

Cross kontrola: `tests/runtime/cross_linux.py --target arm64|rv64 --backend
src/luxir --backend build/luxir`; vyžaduje cross GCC a qemu-user. Výsledky
QEMU nepotvrzují výkon na skutečném CPU ani self-host cross LSL compileru.

## Obecné TLS, vlákna, registry a Linux signály

Tyto služby nejsou vázané na jazyk LSL. Libovolný frontend může emitovat
stejné IR a využít stejné ABI; jazyková sémantika se předává
přes explicitní kontrakt a metadata. Plánovaný standardní managed runtime
a jeho objektové ABI spravuje LuxIR; frontend se tomuto ABI přizpůsobí,
neimplementuje strojový GC ani platformní runtime. Runtime ABI je stále
`none`: externí moduly se sestavují a linkují spolu s programem, nikoli
automaticky příkazem LuxIR do hotové binárky.

`tls.c/.lir`, `tls.h` a `linux-*/tls.S`: statické ELF local-exec TLS pro
x86_64, arm64 a rv64. `luxrt_tls_template_from_stack` čte PT_TLS z auxv
platného kernel stacku; podporuje PT_PHDR nebo standardní ELF64 obraz s
phdr hned za 64bajtovou hlavičkou a PT_LOAD od offsetu 0. Jiný layout vrací
-ENOTSUP. Není to parser nedůvěryhodných ELF souborů. Segment TLS musí mít
zarovnanou vaddr, filesz <= memsz a power-of-two align nejvýše 1 MiB.
`allocate_native` mapuje vlastní blok, kopíruje inicializovaná data a
ponechá zero-fill; hlavní vlákno programu bez libc volá `luxrt_tls_main`
před prvním `thread(symbol)`. Neměňte TLS hlavního vlákna programu s libc:
libc má vlastní TCB, DTV, errno a další interní data.

`thread_spawn_tls(fn,arg,stack,size,tid,tp)` nastavuje CLONE_SETTLS; TP
pochází z `allocate_native`. Caller spravuje stack/guard, tid slot a TLS
blok; žádnou z těchto pamětí neuvolní před join. `tls_release` odmítá
uvolnění TLS právě běžícího vlákna (-EBUSY), ale nekontroluje ostatní
vlákna: jejich životnost je povinnost volajícího. Nejsou podporovány
dlopen, dynamic TLS, TLS destructors, libc v raw callbacku ani unwinding.
Legacy spawn nemění původní kontrakt. Testy spouštějí 40 callbacků z IR
s initialized a 64-byte-aligned zero-fill TLS a samostatný static PIE bez libc.

`context.h`, `linux-*/context.S`: `luxrt_capture_regs(snapshot)` ukládá
callee-saved GP registry, callee-saved floating raw bits (ARM64/RV64) a
caller SP; reserve 25 uint64 words, skutečný počet vrací `context_words`.
Nejde o longjmp, unwinding ani hotový GC. Caller prochází uložené registry
**i živý stack**, řeší zastavení ostatních vláken a uchování snapshotu.
`startup_stack_top` vrací původní kernel stack pointer, který caller
předá z entry (nevytváří stack mapy). Testovací conservative mark scanner
ověřuje alokaci zachovanou v jediném callee-saved registru fixture.

`signal.h`, `linux-*/signal.c/.lir/.S`: descriptor 24 bytes (handler,
Linux flags, 64b kernel mask) nezávislý na libc struct sigaction. Action
0 znamená default, 1 ignore; query používá NULL action. X86/ARM vkládají
vlastní restorer, RV64 používá kernel/VDSO návrat. Vrací raw negativní
errno, při chybě action output nemění. Handler musí být async-signal-safe;
SA_SIGINFO vyžaduje callback odpovídající Linux tříargumentové signatuře
(a vhodný cast v C), nikoli obyčejný handler(int). Sigaltstack a přenosná
Windows/macOS signal politika zatím nejsou součástí tohoto modulu.

Kontroly: `tests/runtime/run_tls.py`, `tls_entry.py`, `run_platform.py`;
`--backend src/luxir --backend build/luxir --target amd64_sysv|arm64|rv64`.
ARM64/RV64 execution je v QEMU, nikoli měření výkonu skutečného CPU.


`relocate.c/.lir` a `entry-relocate.S` poskytují startup pro trusted ELF64
static PIE: RELA s architekturním R_RELATIVE (žádné symbol/IFUNC relokace)
a následný mprotect GNU_RELRO na celé pokryté stránky. Musí běžet před TLS
kopírováním a před prvním aplikačním kódem/vláknem. Driver je automaticky
připojuje. Původní `entry.S` bez tohoto kroku zůstává pro dosavadní omezené
programy bez dynamických datových relokací. Nejde o obecný dynamický loader.

Oprava malých TLS bloků: zarovnání TCB a velikost variant-II obrazu jsou
odlišné veličiny. Obraz se zaokrouhluje podle původního PT_TLS alignment,
TP nejméně na 16 bajtů. Zvýšení alignment obrazu z 8 na 16 posouvalo data
oproti linkerovým TPOFF offsetům. Driver testuje align 1..64 a krátký BSS.

## Arena nad pamětí volajícího

`arena.h/.c/.lir` poskytují jednoduchý 24bajtový raw alokátor bez OS
závislostí: `{ Ptr base; UInt64 capacity; UInt64 used; }`, zarovnání polí 8.
`luxrt_arena_init(arena, base, capacity)` ověří null rozsah a přetečení
adresy a nastaví used na nulu. `luxrt_arena_alloc(arena, size, alignment,
out)` přidělí nezávislý blok s požadovaným zarovnáním (nenulová mocnina
dvou), bez inicializace obsahu. Nulová velikost je chyba.

Status: 0 úspěch, -22 neplatný parametr, -75 přetečení rozsahu při init,
-12 nedostatečná kapacita. Při chybě se nemění descriptor ani výstupní slot.
`luxrt_arena_rewind(arena, mark)` vrací kurzor k dřívějšímu snímku used;
mark musí patřit stejné životnosti arény. Hodnota větší než used je -22.
Rewind zneplatní novější bloky, nemaže jejich obsah a nevolá destruktory.

Descriptor, backing paměť a výstupní sloty musí být platné a disjunktní;
descriptor mění pouze toto API. Caller musí zajistit skutečnou velikost
a životnost backing paměti. Každá aréna má jediného vlastníka; pro
souběžná vlákna použijte oddělené arény nebo vnější synchronizaci.

Raw Linux driver připojuje modul automaticky. V IR lze descriptor
vytvořit `Ptr(arena) = alloc8(24)` a předat mu backing rozsah. Samotné
přidělení neprovádí syscall, nevede tabulku objektů a neskenuje kořeny.
Jde o obecné primitivum pro dočasná data nebo budoucí allocator runtime;
není to dokončený managed alokátor, generační GC ani správa rootů.

Testy: `tests/runtime/run_arena.py` — nezávislé 128bitové zaokrouhlování
adres, náhodné délky/zarovnání, canaries, nezměněný stav při chybě,
obsah se při alokaci nemaže, C ASan/UBSan, C/LSL shoda assembleru šesti
cílů a skutečné standalone execution tří Linux cílů.
Benchmark `arena_benchmark.py` měří volání, kontroly a pointer checksum;
nezahrnuje mapování, inicializaci objektů ani GC.

## Float64 bez libc

`float64.h/.c/.lir` přijímají/vracejí IEEE64 bity jako UInt64. Frontend
může použít `bitcast` pro Float64 registr. Parser je locale-free strict
ASCII: volitelné +/-, číslice s nejvýše jednou tečkou, volitelné e/E
s exponentem. Bez whitespace, podtržítek a hex; lower-case inf/infinity
a nan přijímá explicitně. Overflow je signed infinity, underflow signed
zero. Zaokrouhlení nearest ties-even; chyby -22 syntax a -34 neúměrný
span zachovají output. Raw paměťové rozsahy musí být platné.

Krátké vstupy (do 17 significant digits) používají Ryu s2d. Delší
vstupy mají přesný fallback: první 800 relevantních číslic, count/exponent
a sticky tail; porovnání s exaktním desetinným zápisem binárních
midpointů. Žádný midpoint IEEE64 nemá více než tuto hranici číslic.
Prefix je jen hint; rozhodnutí ties/overflow/subnormal provede exaktní
integer porovnání. Není to hotový Eisel-Lemire fast path.

Formatter používá nejkratší Ryu digits a Python-repr prahy fixed/scientific
(-4 až 15), .0 pro fixed integer, signed zero, lower-case inf/nan,
sign exponentu a nejméně dvě exponentové číslice. Vrátí byte count nebo
-28 při malé kapacitě; nepíše NUL a při chybě nemění buffer.

Modul se v raw driveru připojí automaticky a nepoužité funkce/tabulky se
vyřadí. Vendored Ryu je pinovaný a s licencemi v `runtime/vendor/ryu`.
Generated IR používá skutečné `umulh` a `clz`, žádný externí intrinsics
helper. Ryu C reference používá uint128; běhový program z IR C nepotřebuje.
