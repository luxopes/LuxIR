# LuxIR TODO

Cíl: frontend (LSL) řeší jen jazyk a zapíše jednotný LuxIR. Vše strojové a
platformní řeší LuxIR: optimalizace, generování kódu, runtime jazyka,
přístup k operačnímu systému, formát binárky a linkování. Jeden `.lir` z LSL
pak dá spustitelný program pro Linux, Windows, macOS i Android.
Navazující plán frontendu: sekce „Plán 0.10“ v `TODO.md` repozitáře
LuxAI/LSL.

Zdrojem pravdy zůstává C reference v `src/`; `lsl/luxir.lsl` se přegeneruje
(`make lsl`). Každá položka platí až s testy v obou verzích (C i LSL dají
stejný výstup) a s měřením: programy LSL přeložené přes LuxIR nesmí být
pomalejší než předchozí vydání LSL, při překladu ani za běhu.

## Standardní runtime pro frontendy (LSL) — 13. 10. 2026

Rozhodnutí: LSL bude jen frontend (LSL → LuxIR). Celý runtime patří LuxIR:
model objektů, alokace a GC, kolekce, řetězce, převody čísel, chyby, I/O,
procesy, vlákna, pro každou architekturu a systém. Rozhraní standardizuje
LuxIR, LSL se mu přizpůsobí. Požadavky a dnešní rozhraní (144 funkcí):
`doc/lsl-runtime.md`; referenční kód slovníku a řetězců v LuxIR:
`doc/lsl-runtime/`; referenční chování: x86 emitory runtime v repozitáři LSL.

- [x] **Runtime LSL v LuxIR (0.1.2):** zdroje runtime převzaté z LSL do
  `runtime/lsl/`, sestavení `make runtime-lsl`, ABI `runtime_abi "lsl-0"`
  (dnešní rozhraní LSL beze změny) a `luxir -x`: z `.lir` spustitelný
  program (GNU as/ld z pevných cest, runtime vedle `luxir`). Objekt i
  binárky LSL bajtově shodné s dosavadní cestou; testy `tests/link.py`.
- [ ] **Předání LSL:** LSL zapíše hlavičku `runtime_abi "lsl-0"`, volá
  `luxir -x` a odstraní své kopie emitorů runtime, `runtime.o`,
  `qbe_runtime_dump` a přímý backend; instaluje `lib/luxir/lsl-0/`.
  Ověřeno ve worktree LSL (VALIDATION.md), v LSL zatím necommitnuto.
- [ ] **Návrh stabilního rozhraní (`lsl-1`):** tagované hodnoty, layouty,
  chyby, GC kořeny a mapy, seznam funkcí; frontend se mu přizpůsobí.
- [ ] **Přepis runtime do přenositelného LuxIR** se stejným chováním a
  výkonem jako emitory x86 (měřítka v `doc/lsl-runtime.md`); emitory pak
  z `runtime/lsl/` zmizí.
- [ ] **Linux arm64 a rv64**, pak Windows a macOS.

## Požadavky LSL pro přesun runtime do LuxIR — 13. 10. 2026

LSL přepisuje svůj runtime (dnes ~10 000 řádků ručně generovaného x86-64:
GC, alokátor, řetězce, kolekce, převody, I/O, procesy, vlákna) do LSL/LuxIR
nad luxrt, aby LSL generovalo jen přenositelný LuxIR. Ověřeno proti
`doc/ir.md` a `runtime/` verze 0.1.1 a proti tomu, co runtime LSL používá.
Bez blokujících položek nejde runtime na Linuxu přeportovat; ostatní mají
pomalejší náhradu, kterou LSL do té doby použije.

Blokující (Linux x86_64, arm64, rv64):
- [ ] **TLS ve vláknech:** `thread` data fungují pro hlavní vlákno, ale
  vlákna z luxrt nemají TLS blok. Runtime LSL drží v TLS čekající chybu a
  její příznak (zapisuje se z I/O, procesů, haldy) a vrchol zásobníku vlákna.
  Potřeba: vytvoření vlákna s inicializovaným statickým TLS (fs / tpidr_el0 /
  tp), přístup přes `thread(x)` v obou vláknech.
  Přijetí: test s 40 vlákny, každé zapisuje a čte vlastní `thread` proměnnou.
- [ ] **Uložení registrů a ukazatel zásobníku:** konzervativní GC LSL musí
  najít ukazatele i v registrech. Potřeba primitivum (instrukce nebo luxrt
  funkce pro každou architekturu, obdoba setjmp), které uloží callee-saved
  registry do paměti a vrátí aktuální ukazatel zásobníku; plus vrchol
  zásobníku hlavního vlákna ze startu.
  Přijetí: GC test, kde je jediný odkaz na objekt v callee-saved registru.
- [ ] **Signály:** `rt_sigaction` s `sa_restorer` (návratová rutina pro každou
  architekturu), maska signálů; runtime LSL instaluje obsluhu při startu.

Výkonové (náhrada existuje, ale je pomalejší):
- [ ] **Atomiky jako instrukce:** atomické add/xchg/cmpxchg/load/store a
  bariéry v IR (dnes jen volání do `luxrt/*/atomic.S`). Používají je zámky,
  vlákna, GC a vestavěné `atomic_add`, `atomic_xchg`, `atomic_cas`.
- [ ] **`sqrt`** pro Float64 (a Float32) jako instrukce; vestavěné `sqrt`
  a modul `math`.
- [ ] **Horní polovina násobení 64×64 → 128** (`umulh`, `mulh`): přesný
  převod textu na Float (Eisel-Lemire), hashe. Náhrada: čtyři 32bitová násobení.
- [ ] **clz, ctz, popcount** jako instrukce: vestavěné `popcount`,
  `trailing_zeros`, převody Float.
- [ ] **Inlinování** malých funkcí (už v sekci Optimalizace): runtime volá
  drobné pomocné funkce v horkých smyčkách (hash, porovnání, kontrola typu).

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
- [ ] Číslované ABI integrované runtime knihovny luxrt a kontrola při
  linkování. Dnešní `none` výslovně označuje externí runtime; nejde o
  implementaci luxrt ani ověření kompatibility externí knihovny.
- [ ] Popis layoutu objektů frontendu: ukazatelové mapy typů, kořeny,
  safepointy. Frontend řekne *co* je ukazatel, LuxIR rozhodne *jak* GC
  prochází zásobník a registry (stack mapy).
- [ ] „Volání, které může selhat“: chybová cesta z volání (landing pad nebo
  druhá návratová hodnota), aby frontend negeneroval kontrolu příznaku po
  každém volání.
- [ ] Kontrolovaná aritmetika jako instrukce (přetečení Int, dělení nulou,
  kontrola mezí) s cílem chyby; backend kontroly slučuje a odstraňuje.
- [ ] Dokumentovat celé rozhraní v `doc/runtime.md` vedle `doc/ir.md`.

## 32bitové cíle s assemblerem (arm, i386) — 16. 10. 2026

- [x] `w32.c`: rozklad Kl na dvojice slov před ABI, pomocné funkce
  `runtime/w32/helpers.lir`, druhé kolo gvn/gcm nad slovy, identity,
  větvení 64bitových porovnání, přenos `addc`/`adc`, `subc`/`sbc`.
- [x] Backendy `arm` (AAPCS hard-float) a `i386` (cdecl, SSE2, x87 jen
  u výsledku). Testy `tests/run_w32.py` a rozdílový fuzzer
  `tests/w32_diff.py` proti amd64 (`make check-w32`).
- [ ] Výkon: na i386 je kód LSL 1,2–2,2× pomalejší než C z `c32` přes
  clang -O2 (málo registrů, dvojice). Dál: výběr adresních režimů na arm
  ([reg, reg, lsl #n]), shlukování horních slov (ukazatele), lepší
  přidělení dvojic, `umull`/`mul` pro 32×32→64.
- [ ] Desetinná čísla Float32 jako argumenty na arm: každé zabírá celý
  registr d (bez dosazování do děr v s0-s15 podle AAPCS); d7 je pomocný
  registr, desetinné argumenty jdou jen v d0-d6.

## Výstup objektů a linkování (bez binutils)

- [x] Vlastní assembler do objektů x86-64 a i386 ELF (`luxir -a`,
  src/as/): bajtově shodný s GNU as na výstupu LuxIR i vrstvách
  architektury LSL (16. 10. 2026).
- [ ] Assembler pro arm, arm64 a rv64 (ELF), pak COFF (Windows) a Mach-O
  (x86_64 a arm64) jako výstupní formáty.
- [x] Vlastní linker ELF (`luxir -l`, src/ld/): statický PIE x86-64 a
  statický i386 se zárukami dnešního `ld` v LSL (`-z text`, `-z
  noexecstack`, gc-sections, bez build-id, strip); LSL na Linuxu x86 už
  binutils nepotřebuje (16. 10. 2026).
- [x] Assembler a linker AArch64 ELF (`-t arm64`): bajtově shodné objekty
  s GNU as na překladači LSL i runtime, relokace adrp/:lo12:/skoků;
  LSL linux-arm64 bez binutils (19. 10. 2026).
- [x] Assembler a linker RISC-V ELF (`-t rv64`, rv64g bez relaxace):
  bajtově shodné objekty s GNU as na překladači LSL i runtime včetně
  relaxace větví a rozkladu `li`; LSL linux-riscv64 bez binutils
  (19. 10. 2026). Baremetal-riscv64 zůstává na GNU ld (linker skript).
- [x] Assembler a linker ARM (32bit, `-t arm`, A32 ARMv7-A s VFPv3-D16,
  objekty REL): bajtově shodné objekty s GNU as na překladači LSL,
  runtime i pomocných funkcích w32; LSL linux-arm bez binutils
  (19. 10. 2026).
- [ ] PE32+ s ASLR, DEP a high-entropy VA, importy `kernel32` a `ws2_32`;
  Mach-O s `LC_MAIN`, `libSystem` a ad-hoc podpisem (na arm64 povinný).
- [ ] `luxir -t <cíl> -o program program.lir runtime.lir …`: od `.lir` po
  spustitelný soubor jedním příkazem, bajtově deterministicky.
- [ ] Odstranění mrtvého kódu celého obrazu včetně runtime (dnes se
  `runtime.o` linkuje celý; tabulka mocnin pro Float zvětšila v LSL 0.9.2
  každou binárku o ~12 KB).
- [x] Akceptace: linuxové binárky LSL bez `as` a `ld` projdou branou
  hardeningu obrazu (tools/qbe/verify_image.py elf_contract) a celou sadou
  testů (x86-64 pevný bod a plná sada, linux-x86 106/106).

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
  inkrementů; C/LSL shoda. Callback nesmí používat libc/TLS/managed objekty.
- [x] rv64 PC-relative adresy pro static PIE přes explicitní `-p`:
  lokální i externí symboly + offset, C/LSL shoda a spuštění v QEMU.
  Dynamické datové relokace a vlastní linker tím nejsou implementovány.
- [x] Linux x86_64 self-host stage 1 == stage 2 == stage 3: 48,27 s a
  46,16 s, obě pod limitem 60 s v tomto prostředí; `tests/selfhost.py`.
- [x] Linux lokální testová matice, C/LSL execution arm64/rv64 + assembler
  šesti targetů: `tools/check_linux.sh`. Nepotvrzuje výkon skutečných CPU.
- [ ] Spustit novou `.forgejo/workflows/linux-reference.yml` na remote CI.
  Workflow je připraven a YAML validní, remote job zatím neběžel; ověřuje C
  reference, nikoli vlastní self-host na všech platformách.
- [ ] Integrovat tyto moduly s aktuálním frontendem/layoutem LSL, doplnit
  výkonnostní brány a skutečné cílové execution testy; nenahrazovat celou
  širší položku níže stavem hotovo podle samotné existence prototypu.

## Runtime knihovna LuxIR (luxrt)

### OS vrstva: jediné místo s kódem pro konkrétní systém
- [ ] Paměť: map, unmap, protect, velikost stránky.
- [ ] Soubory a adresáře: open, read, write, seek, close, stat, list,
  rename, unlink, mkdir, chmod, symlink, cesta k programu.
- [ ] Procesy: spawn s argv, env a pipes; wait, exit, kill; zavírání
  zděděných deskriptorů.
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

- [ ] Inlinování malých funkcí (listové funkce, tělo jeden výraz). Benchmark
  LSL `function_calls` je v 0.9.2 omezen latencí výpočtu; zbývá ~1 takt volání
  na iteraci.
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
- [ ] Výkonové měření backendu: čas překladu LSL compileru přes LuxIR
  nesmí růst s přidanými optimalizacemi bez vyrovnání jinde.
