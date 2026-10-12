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
Callback nesmí používat libc, compiler TLS, unwinding nebo managed objekty;
TLS ani GC neexistuje. Tento modul není plnohodnotné vlákno jazyka LSL.
Guard page poskytuje caller; test má guarded stacky a čtyři miliony inkrementů.

Cross kontrola: `tests/runtime/cross_linux.py --target arm64|rv64 --backend
src/luxir --backend build/luxir`; vyžaduje cross GCC a qemu-user. Výsledky
QEMU nepotvrzují výkon na skutečném CPU ani self-host cross LSL compileru.
